#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SMF_BRIDGE_INTERNAL
#include "camera_bridge.h"
#include "camera_control.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace camera_bridge {
    namespace {

        // smf_bridge_main_state.flags
        constexpr uint32_t flag_eligible = 1;
        constexpr uint32_t flag_owned = 2;
        constexpr uint32_t flag_motion_blocked = 4;
        constexpr uint32_t flag_text_captured = 8;
        constexpr uint32_t flag_search_focused = 16;

        SRWLOCK incoming_gate = SRWLOCK_INIT;
        SRWLOCK outgoing_gate = SRWLOCK_INIT;
        SRWLOCK path_gate = SRWLOCK_INIT;
        smf_bridge_main incoming{};
        smf_bridge_status outgoing{sizeof(smf_bridge_status), 2};
        bool has_incoming = false;
        wchar_t kernel_path[1024]{};
        uint32_t kernel_characters = 0;
        std::atomic<uint64_t> fence{0};
        std::atomic<uint64_t> fault_epoch{0};
        std::atomic<uint32_t> main_thread{0};

        struct try_lock {
            SRWLOCK* value;

            explicit try_lock(SRWLOCK& lock) : value(TryAcquireSRWLockExclusive(&lock) ? &lock : nullptr) {}
            ~try_lock() {
                if (value) ReleaseSRWLockExclusive(value);
            }

            explicit operator bool() const {
                return value != nullptr;
            }
        };

        // The first caller becomes the main thread; every later call must come from it.
        HRESULT require_main_thread() {
            const uint32_t id = GetCurrentThreadId();
            uint32_t expected = 0;
            main_thread.compare_exchange_strong(expected, id);
            return main_thread.load() == id ? S_OK : E_UNEXPECTED;
        }

        int64_t now() {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return value.QuadPart;
        }

        bool finite(double x) {
            return std::isfinite(x);
        }

        bool valid_trajectory(const smf_camera_trajectory& t) {
            if (!t.id) {
                const smf_camera_trajectory zero{};
                return std::memcmp(&t, &zero, sizeof(t)) == 0;
            }

            return t.id <= INT64_MAX && t.kind == 1 && !t.flags &&
                finite(t.start_seconds) && t.start_seconds >= 0 &&
                finite(t.duration_seconds) && t.duration_seconds >= 0 &&
                finite(t.source_x) && finite(t.source_z) && finite(t.source_root_size) && t.source_root_size > 0 &&
                finite(t.target_x) && finite(t.target_z) && finite(t.target_root_size) && t.target_root_size > 0;
        }

        bool valid_pose(const smf_camera_pose& p) {
            return p.version == 2 && p.size == sizeof(p) && p.session && p.epoch && p.pose_sequence && p.map_id >= 0 &&
                !p.reserved && !(p.pan_flags & ~SMF_CAMERA_PAN_COMPLETED) &&
                p.active_pan_id <= INT64_MAX && p.finished_pan_id <= INT64_MAX &&
                (!p.pan_flags || p.finished_pan_id) &&
                finite(p.x) && finite(p.z) && finite(p.root_size) && p.root_size > 0 &&
                finite(p.projection_half_height) && p.projection_half_height > 0;
        }

        struct kernel_module {
            HMODULE module = nullptr;
            decltype(&smf_camera_create) create = nullptr;
            decltype(&smf_camera_adopt) adopt = nullptr;
            decltype(&smf_camera_configure) configure = nullptr;
            decltype(&smf_camera_step) step = nullptr;
            decltype(&smf_camera_release) release = nullptr;

            template <class T>
            bool resolve(T& target, const char* name) {
                auto address = GetProcAddress(module, name);
                static_assert(sizeof(address) == sizeof(target), "function pointer width");
                std::memcpy(&target, &address, sizeof(target));
                return target != nullptr;
            }

            HRESULT load() {
                if (module) return create && adopt && configure && step && release ? S_OK : E_NOINTERFACE;

                wchar_t path[1024]{};
                {
                    try_lock lock(path_gate);
                    if (!lock || !kernel_characters) return S_FALSE;
                    std::memcpy(path, kernel_path, (size_t(kernel_characters) + 1) * sizeof(wchar_t));
                }

                // Kept loaded until process exit even when the exports turn out to be missing.
                module = LoadLibraryW(path);
                if (!module) return HRESULT_FROM_WIN32(GetLastError());

                const bool resolved = resolve(create, "smf_camera_create") && resolve(adopt, "smf_camera_adopt") &&
                    resolve(configure, "smf_camera_configure") && resolve(step, "smf_camera_step") &&
                    resolve(release, "smf_camera_release");
                return resolved ? S_OK : E_NOINTERFACE;
            }
        };

        struct worker_state {
            kernel_module kernel;
            smf_bridge_main main{};
            smf_bridge_status status{sizeof(smf_bridge_status), 2};
            smf_camera_pose pose{};
            smf_bridge_desired prepared{};
            bool have_main = false;
            bool previous_middle = false;
            bool prepared_valid = false;
            bool prepared_publish = false;
            double previous_x = 0;
            double previous_y = 0;
            double seed_size = 0;
            uint64_t input_sequence = 0;
            uint64_t settings_revision = 0;
            int32_t keys[8]{};

            void report() {
                status.fence_epoch = fence.load(std::memory_order_acquire);
                status.main_thread = main_thread.load();

                try_lock lock(outgoing_gate);
                if (lock) {
                    outgoing = status;
                } else {
                    status.mailbox_drops++;
                }
            }

            // A camera fault stops camera updates; it never asks the compositor to detach.
            HRESULT fault(int32_t error) {
                fault_epoch.store(have_main ? main.state.epoch : 0, std::memory_order_release);
                status.state = bridge_fault;
                status.result = error;
                previous_middle = false;
                prepared_valid = false;
                prepared_publish = false;
                status.desired = {};

                report();
                return S_FALSE;
            }
        };

        // Owned by the compositor worker thread. Leaked on purpose: no CRT shutdown code may
        // call into the NativeAOT kernel.
        worker_state& worker = *new worker_state();

        int virtual_key(int code) {
            if (!code) return 0;
            if (code >= 'a' && code <= 'z') return 'A' + code - 'a';
            if (code >= '0' && code <= '9') return code;
            if (code >= 256 && code <= 265) return VK_NUMPAD0 + code - 256;
            if (code >= 282 && code <= 296) return VK_F1 + code - 282;

            switch (code) {
            case 273: return VK_UP;
            case 274: return VK_DOWN;
            case 275: return VK_RIGHT;
            case 276: return VK_LEFT;
            case 303: return VK_RSHIFT;
            case 304: return VK_LSHIFT;
            case 305: return VK_RCONTROL;
            case 306: return VK_LCONTROL;
            case 307: return VK_RMENU;
            case 308: return VK_LMENU;
            case 32: return VK_SPACE;
            case 9: return VK_TAB;
            case 27: return VK_ESCAPE;
            default: return -1;
            }
        }

        bool resolve_bindings(const smf_bridge_bindings& source, int32_t (&keys)[8]) {
            const int32_t input[] = {source.up, source.up2, source.down, source.down2,
                source.left, source.left2, source.right, source.right2};

            for (int i = 0; i < 8; ++i) {
                keys[i] = virtual_key(input[i]);
                if (keys[i] < 0) return false;
            }
            return true;
        }

        bool held(int key) {
            return key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
        }

        bool pair_held(const int32_t* keys, int index) {
            return held(keys[index]) || held(keys[index + 1]);
        }

        smf_bridge_desired desired_from_pose(const smf_camera_pose& p) {
            return {2, sizeof(smf_bridge_desired), p.epoch, p.pose_sequence, p.map_id, 0, p.x, p.z, p.root_size,
                p.projection_half_height, p.active_pan_id, p.finished_pan_id, p.pan_flags, 0};
        }

    }

    void worker_ready(uint32_t thread, int64_t frequency) {
        worker.status.worker_thread = thread;
        worker.status.qpc_frequency = frequency;
        worker.status.state = bridge_waiting;
        worker.report();
    }

    HRESULT worker_prepare(bool focused, bool pointer_valid, double x, double y, bool middle, smf_bridge_desired& target) {
        worker_state& w = worker;
        w.prepared_valid = false;
        w.prepared_publish = false;
        if (GetCurrentThreadId() != w.status.worker_thread || w.status.qpc_frequency <= 0) return E_UNEXPECTED;

        {
            try_lock lock(incoming_gate);
            if (lock && has_incoming && (!w.have_main || incoming.publication > w.main.publication)) {
                w.main = incoming;
                w.have_main = true;
                w.status.main_revision = incoming.publication;
            }
        }

        const uint64_t current_fence = fence.load(std::memory_order_acquire);
        if (!w.have_main || !current_fence || w.main.state.epoch != current_fence) {
            w.status.state = bridge_waiting;
            w.status.desired = {};
            w.previous_middle = false;
            w.report();
            return S_FALSE;
        }

        const auto& main = w.main.state;
        w.status.applied_sequence = main.applied_sequence;

        if ((main.flags & flag_eligible) == 0) {
            // Not eligible: show the pose main published, never one derived from the image.
            w.status.state = bridge_waiting;
            w.status.desired = {};
            w.previous_middle = false;

            if (main.map_id >= 0 && finite(main.x) && finite(main.z) && finite(main.root_size) && main.root_size > 0 &&
                finite(main.projection_half_height) && main.projection_half_height > 0) {
                w.prepared = {2, sizeof(smf_bridge_desired), current_fence, 0, main.map_id, 0, main.x, main.z, main.root_size,
                    main.projection_half_height, 0, 0, 0, 0};
                w.prepared_valid = true;
                target = w.prepared;
                w.report();
                return S_OK;
            }

            w.report();
            return S_FALSE;
        }

        const HRESULT loaded = w.kernel.load();
        if (loaded == S_FALSE) {
            w.report();
            return S_FALSE;
        }

        if (FAILED(loaded)) return w.fault(loaded);
        if (!resolve_bindings(w.main.bindings, w.keys)) return w.fault(E_NOTIMPL);

        const int64_t now_qpc = now();
        const double seconds = double(now_qpc) / w.status.qpc_frequency;

        if (!w.status.kernel_session || w.status.worker_epoch != current_fence) {
            smf_camera_init init{2, sizeof(smf_camera_init), current_fence, main.map_id, 0,
                main.x, main.z, main.root_size, seconds};

            const int32_t result = w.status.kernel_session
                ? w.kernel.adopt(w.status.kernel_session, &init, sizeof(init), &w.main.settings, sizeof(w.main.settings), &w.pose, sizeof(w.pose))
                : w.kernel.create(&init, sizeof(init), &w.main.settings, sizeof(w.main.settings), &w.pose, sizeof(w.pose));
            if (result != SMF_CAMERA_OK) return w.fault(result);
            if (!valid_pose(w.pose)) return w.fault(E_INVALIDARG);

            w.status.kernel_session = w.pose.session;
            w.status.worker_epoch = current_fence;
            w.status.seed_sequence = w.pose.pose_sequence;
            w.status.state = bridge_seed;
            w.status.result = S_OK;
            w.status.adopts++;
            w.input_sequence = 0;
            w.settings_revision = w.pose.settings_revision;
            w.seed_size = w.pose.root_size;
            w.previous_middle = false;
        }

        if (w.status.state == bridge_fault) {
            w.report();
            return S_FALSE;
        }

        if (main.map_id != w.pose.map_id || main.applied_sequence > w.pose.pose_sequence) return w.fault(E_INVALIDARG);
        if (w.main.settings.revision != w.settings_revision) {
            const int32_t result = w.kernel.configure(w.status.kernel_session, current_fence,
                &w.main.settings, sizeof(w.main.settings), &w.pose, sizeof(w.pose));
            if (result != SMF_CAMERA_OK) return w.fault(result);
            if (!valid_pose(w.pose)) return w.fault(E_INVALIDARG);

            w.settings_revision = w.pose.settings_revision;
            w.status.configs++;
        }

        const bool acknowledged = (main.flags & flag_owned) != 0 && main.applied_sequence >= w.status.seed_sequence;
        if (acknowledged) {
            const bool blocked = !focused || !pointer_valid || (main.flags & flag_motion_blocked) != 0;
            const bool keyboard_blocked = blocked || (main.flags & (flag_text_captured | flag_search_focused)) != 0;

            smf_camera_input input{2, sizeof(smf_camera_input), current_fence, ++w.input_sequence,
                w.settings_revision, 0, 0, seconds};
            input.trajectory = w.main.trajectory;
            input.pointer_x = pointer_valid ? x : 0;
            input.pointer_y = pointer_valid ? y : 0;
            if (blocked) input.flags |= SMF_CAMERA_MOTION_BLOCKED;

            if (!keyboard_blocked) {
                input.pan_x = pair_held(w.keys, 6) ? 1 : pair_held(w.keys, 4) ? -1 : 0;
                input.pan_z = pair_held(w.keys, 2) ? -1 : pair_held(w.keys, 0) ? 1 : 0;
            }

            if (!blocked && held(VK_LSHIFT)) input.flags |= SMF_CAMERA_FAST_PAN;
            if (!blocked) {
                if (middle && w.previous_middle) {
                    input.drag_x = x - w.previous_x;
                    input.drag_y = y - w.previous_y;
                }
                if (!middle && w.previous_middle) input.flags |= SMF_CAMERA_MIDDLE_RELEASED_PULSE;
            }

            w.previous_middle = !blocked && middle;
            w.previous_x = x;
            w.previous_y = y;

            camera_control::prepare(current_fence, blocked, x, y, input);
            const int32_t result = w.kernel.step(w.status.kernel_session, &input, sizeof(input), &w.pose, sizeof(w.pose));
            if (result != SMF_CAMERA_OK) return w.fault(result);
            if (!valid_pose(w.pose)) return w.fault(E_INVALIDARG);

            w.status.steps++;
            w.status.step_qpc = now_qpc;
            w.status.state = blocked ? bridge_blocked : bridge_owned;
            w.status.flags = (keyboard_blocked ? 1u : 0u) | (blocked ? 2u : 0u) | (middle ? 4u : 0u);
        } else {
            // Until main acknowledges the seed only the kernel clock advances: no held input,
            // no trajectory, no zoom or bounds clamping, so the seed pose stays exactly as published.
            smf_camera_input idle{2, sizeof(smf_camera_input), current_fence, ++w.input_sequence,
                w.settings_revision, SMF_CAMERA_CLOCK_ONLY, 0, seconds};

            const int32_t result = w.kernel.step(w.status.kernel_session, &idle, sizeof(idle), &w.pose, sizeof(w.pose));
            if (result != SMF_CAMERA_OK) return w.fault(result);
            if (!valid_pose(w.pose)) return w.fault(E_INVALIDARG);
            if (w.pose.root_size != w.seed_size || w.pose.pose_sequence != w.status.seed_sequence) return w.fault(E_NOTIMPL);

            w.status.state = bridge_seed;
            w.previous_middle = false;
        }

        if (fence.load(std::memory_order_acquire) != current_fence) {
            w.status.desired = {};
            w.previous_middle = false;
            w.report();
            return S_FALSE;
        }

        w.prepared = desired_from_pose(w.pose);
        w.prepared_valid = true;
        w.prepared_publish = true;
        target = w.prepared;
        return S_OK;
    }

    void worker_committed(HRESULT result) {
        worker_state& w = worker;
        if (GetCurrentThreadId() != w.status.worker_thread) return;

        if (FAILED(result)) {
            w.fault(result);
            return;
        }

        if (result == S_OK && w.prepared_valid && w.prepared.epoch == fence.load(std::memory_order_acquire)) {
            w.status.commit_sequence++;
            w.status.commit_qpc = now();
            w.status.desired = w.prepared_publish ? w.prepared : smf_bridge_desired{};
        }

        w.prepared_valid = false;
        w.prepared_publish = false;
        w.report();
    }

    void worker_reused_pose() {
        worker_state& w = worker;
        if (GetCurrentThreadId() != w.status.worker_thread) return;

        if (w.prepared_valid && w.prepared.epoch == fence.load(std::memory_order_acquire)) {
            w.status.desired = w.prepared_publish ? w.prepared : smf_bridge_desired{};
        }

        w.prepared_valid = false;
        w.prepared_publish = false;
        w.report();
    }

    void worker_removed() {
        worker_state& w = worker;
        if (GetCurrentThreadId() != w.status.worker_thread) return;

        camera_control::clear();
        if (w.status.kernel_session && w.kernel.release) {
            smf_camera_pose output{};
            const int32_t result = w.kernel.release(w.status.kernel_session, &output, sizeof(output));
            w.status.result = result;
            if (result == 0) w.status.kernel_session = 0;
        }

        w.status.desired = {};
        w.prepared_valid = false;
        w.previous_middle = false;
        w.status.state = bridge_dormant;
        w.report();
    }

}

SMF_BRIDGE_API double __cdecl smf_camera_bridge_now() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return double(camera_bridge::now()) / double(frequency.QuadPart);
}

SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_init(const wchar_t* path, uint32_t characters) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!path || characters < 3 || characters >= 1024) return E_INVALIDARG;
    if (path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return E_INVALIDARG;

    for (uint32_t i = 0; i < characters; ++i) {
        if (!path[i]) return E_INVALIDARG;
    }

    try_lock lock(path_gate);
    if (!lock) return S_FALSE;
    if (kernel_characters) {
        const bool same = kernel_characters == characters && std::memcmp(kernel_path, path, characters * sizeof(wchar_t)) == 0;
        return same ? S_OK : E_INVALIDARG;
    }

    std::memcpy(kernel_path, path, characters * sizeof(wchar_t));
    kernel_path[characters] = 0;
    kernel_characters = characters;
    return S_OK;
}

SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_publish(const smf_bridge_main* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->size != sizeof(*value) || value->version != 2 || !value->publication) {
        return E_INVALIDARG;
    }

    const auto& state = value->state;
    const auto& settings = value->settings;

    if (state.version != 2 || state.size != sizeof(smf_bridge_main_state) || state.reserved || (state.flags & ~63u)) {
        return E_INVALIDARG;
    }
    if (settings.version != 2 || settings.size != sizeof(smf_camera_settings) || !settings.revision ||
        settings.reserved || (settings.flags & ~15u)) {
        return E_INVALIDARG;
    }
    if (state.map_id >= 0 && (!finite(state.projection_half_height) || state.projection_half_height <= 0)) {
        return E_INVALIDARG;
    }
    if (!valid_trajectory(value->trajectory)) return E_INVALIDARG;
    if (value->bindings.version != 1 || value->bindings.size != sizeof(smf_bridge_bindings) || !value->bindings.revision) {
        return E_INVALIDARG;
    }
    if (state.epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;

    try_lock lock(incoming_gate);
    if (!lock) return S_FALSE;
    if (has_incoming && value->publication <= incoming.publication) return E_INVALIDARG;

    incoming = *value;
    has_incoming = true;
    return S_OK;
}

SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_revoke(uint64_t epoch, uint32_t reason) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!epoch || epoch <= fence.load(std::memory_order_acquire) || reason > 10) return E_INVALIDARG;

    fence.store(epoch, std::memory_order_release);
    return S_OK;
}

SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_desired(smf_bridge_desired* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;
    *value = {};

    try_lock lock(outgoing_gate);
    if (!lock) return S_FALSE;
    const uint64_t current_fence = fence.load(std::memory_order_acquire);
    if (outgoing.state == bridge_fault && fault_epoch.load(std::memory_order_acquire) == current_fence) return E_FAIL;
    if (!outgoing.desired.sequence || outgoing.desired.epoch != current_fence) return S_FALSE;

    *value = outgoing.desired;
    return S_OK;
}

SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_status(smf_bridge_status* value, uint32_t bytes) {
    using namespace camera_bridge;
    // Diagnostics may call this from any thread, so it must not claim main-thread ownership.
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;

    try_lock lock(outgoing_gate);
    if (!lock) return S_FALSE;
    *value = outgoing;
    value->fence_epoch = fence.load(std::memory_order_acquire);
    return S_OK;
}

SMF_CONTROL_API smf_camera_control_policy(const smf_control_policy* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;

    return camera_control::publish_policy(*value);
}

SMF_CONTROL_API smf_camera_control_impulse(const smf_control_impulse* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;

    return camera_control::queue(*value);
}

SMF_CONTROL_API smf_camera_control_status(smf_control_status* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(require_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;

    return camera_control::read_status(*value);
}
