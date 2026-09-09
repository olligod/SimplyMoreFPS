#include "camera_bridge.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <dlfcn.h>

namespace camera_bridge {
    namespace {

        // Bits of smf_bridge_main_state::flags.
        constexpr uint32_t flag_eligible = 1;
        constexpr uint32_t flag_owned = 2;
        constexpr uint32_t flag_motion_blocked = 4;
        constexpr uint32_t flag_text_captured = 8;
        constexpr uint32_t flag_search_focused = 16;

        smf_bridge_status empty_status() {
            smf_bridge_status s{};
            s.size = sizeof(s);
            s.version = 2;
            return s;
        }

        std::mutex incoming_gate;
        std::mutex outgoing_gate;
        std::mutex path_gate;
        smf_bridge_main incoming{};
        smf_bridge_status outgoing = empty_status();
        bool has_incoming = false;
        char kernel_path[1024]{};
        uint32_t kernel_characters = 0;
        std::atomic<uint64_t> fence{0};
        std::atomic<uint64_t> fault_epoch{0};
        std::atomic<uint32_t> main_thread{0};

        struct try_lock {
            std::mutex* value;

            explicit try_lock(std::mutex& lock) : value(lock.try_lock() ? &lock : nullptr) {}

            ~try_lock() {
                if (value) value->unlock();
            }

            explicit operator bool() const {
                return value != nullptr;
            }
        };

        // The first main-side caller claims the main thread; every later call must come from it.
        HRESULT on_main_thread() {
            const uint32_t id = native_thread();
            uint32_t expected = 0;
            main_thread.compare_exchange_strong(expected, id);
            return main_thread.load() == id ? S_OK : E_UNEXPECTED;
        }

        int64_t now() {
            return native_now();
        }

        bool is_finite(double x) {
            return std::isfinite(x);
        }

        bool valid_trajectory(const smf_camera_trajectory& t) {
            if (!t.id) {
                const smf_camera_trajectory zero{};
                return std::memcmp(&t, &zero, sizeof(t)) == 0;
            }

            return t.id <= INT64_MAX && t.kind == 1 && !t.flags &&
                is_finite(t.start_seconds) && t.start_seconds >= 0 &&
                is_finite(t.duration_seconds) && t.duration_seconds >= 0 &&
                is_finite(t.source_x) && is_finite(t.source_z) &&
                is_finite(t.source_root_size) && t.source_root_size > 0 &&
                is_finite(t.target_x) && is_finite(t.target_z) &&
                is_finite(t.target_root_size) && t.target_root_size > 0;
        }

        bool valid_pose(const smf_camera_pose& p) {
            return p.version == 2 && p.size == sizeof(p) && p.session && p.epoch && p.pose_sequence && p.map_id >= 0 &&
                !p.reserved && !(p.pan_flags & ~SMF_CAMERA_PAN_COMPLETED) &&
                p.active_pan_id <= INT64_MAX && p.finished_pan_id <= INT64_MAX &&
                (!p.pan_flags || p.finished_pan_id) &&
                is_finite(p.x) && is_finite(p.z) && is_finite(p.root_size) && p.root_size > 0 &&
                is_finite(p.projection_half_height) && p.projection_half_height > 0;
        }

        struct kernel_api {
            void* module = nullptr;
            decltype(&smf_camera_create) create = nullptr;
            decltype(&smf_camera_adopt) adopt = nullptr;
            decltype(&smf_camera_configure) configure = nullptr;
            decltype(&smf_camera_step) step = nullptr;
            decltype(&smf_camera_release) release = nullptr;

            template <class T>
            bool resolve(T& target, const char* name) {
                void* address = dlsym(module, name);
                static_assert(sizeof(address) == sizeof(target), "function pointer width");
                std::memcpy(&target, &address, sizeof(target));
                return target != nullptr;
            }

            HRESULT load() {
                if (module) return create && adopt && configure && step && release ? S_OK : E_NOINTERFACE;

                char path[1024]{};
                {
                    try_lock lock(path_gate);
                    if (!lock || !kernel_characters) return S_FALSE;
                    std::memcpy(path, kernel_path, size_t(kernel_characters) + 1);
                }

                // Stays loaded until process exit, even when its exports turn out to be missing.
                module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                if (!module) return E_NOINTERFACE;

                const bool complete = resolve(create, "smf_camera_create") && resolve(adopt, "smf_camera_adopt") &&
                    resolve(configure, "smf_camera_configure") && resolve(step, "smf_camera_step") &&
                    resolve(release, "smf_camera_release");
                return complete ? S_OK : E_NOINTERFACE;
            }
        };

        struct worker_state {
            kernel_api api;
            smf_bridge_main main{};
            smf_bridge_status status = empty_status();
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

            // Never waits on the mailbox; a busy main side is counted instead.
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

            // A camera fault must not make the compositor detach its layers, hence S_FALSE.
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

            smf_camera_input next_input(uint64_t epoch, double seconds) {
                smf_camera_input input{};
                input.version = 2;
                input.size = sizeof(input);
                input.epoch = epoch;
                input.sequence = ++input_sequence;
                input.settings_revision = settings_revision;
                input.monotonic_seconds = seconds;

                return input;
            }
        };

        // Leaked on purpose: no destructor may call into NativeAOT at shutdown.
        worker_state& worker = *new worker_state();

        // Unity KeyCode to X keysym; -1 means the binding cannot be observed here.
        int keysym_for(int code) {
            if (!code) return 0;
            if (code >= 32 && code <= 126) return code;
            if (code >= 256 && code <= 265) return XK_KP_0 + code - 256;
            if (code >= 282 && code <= 296) return XK_F1 + code - 282;

            switch (code) {
            case 273: return XK_Up;
            case 274: return XK_Down;
            case 275: return XK_Right;
            case 276: return XK_Left;
            case 303: return XK_Shift_R;
            case 304: return XK_Shift_L;
            case 305: return XK_Control_R;
            case 306: return XK_Control_L;
            case 307: return XK_Alt_R;
            case 308: return XK_Alt_L;
            case 9: return XK_Tab;
            case 27: return XK_Escape;
            default: return -1;
            }
        }

        bool resolve_bindings(const smf_bridge_bindings& source, int32_t (&keys)[8]) {
            const int32_t codes[] = {source.up, source.up2, source.down, source.down2,
                source.left, source.left2, source.right, source.right2};
            for (int i = 0; i < 8; ++i) {
                keys[i] = keysym_for(codes[i]);
                if (keys[i] < 0) return false;
            }
            return true;
        }

        bool pair_held(const int32_t* keys, int index) {
            return camera_control::held(keys[index]) || camera_control::held(keys[index + 1]);
        }

        smf_bridge_desired desired_from(const smf_camera_pose& p) {
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
        if (native_thread() != w.status.worker_thread || w.status.qpc_frequency <= 0) return E_UNEXPECTED;

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

        const smf_bridge_main_state& main = w.main.state;
        w.status.applied_sequence = main.applied_sequence;

        if ((main.flags & flag_eligible) == 0) {
            // Not eligible: hand main's own pose back rather than resetting from the source image.
            w.status.state = bridge_waiting;
            w.status.desired = {};
            w.previous_middle = false;

            if (main.map_id >= 0 && is_finite(main.x) && is_finite(main.z) && is_finite(main.root_size) && main.root_size > 0 &&
                is_finite(main.projection_half_height) && main.projection_half_height > 0) {
                w.prepared = {2, sizeof(smf_bridge_desired), current_fence, 0, main.map_id, 0, main.x, main.z,
                    main.root_size, main.projection_half_height, 0, 0, 0, 0};
                w.prepared_valid = true;
                target = w.prepared;
                w.report();
                return S_OK;
            }

            w.report();
            return S_FALSE;
        }

        const HRESULT loaded = w.api.load();
        if (loaded == S_FALSE) {
            w.report();
            return S_FALSE;
        }

        if (FAILED(loaded)) return w.fault(loaded);
        if (!resolve_bindings(w.main.bindings, w.keys)) return w.fault(E_NOTIMPL);

        const int64_t qpc = now();
        const double seconds = double(qpc) / w.status.qpc_frequency;

        if (!w.status.kernel_session || w.status.worker_epoch != current_fence) {
            smf_camera_init init{2, sizeof(smf_camera_init), current_fence, main.map_id, 0,
                main.x, main.z, main.root_size, seconds};

            const int32_t result = w.status.kernel_session
                ? w.api.adopt(w.status.kernel_session, &init, sizeof(init), &w.main.settings, sizeof(w.main.settings), &w.pose, sizeof(w.pose))
                : w.api.create(&init, sizeof(init), &w.main.settings, sizeof(w.main.settings), &w.pose, sizeof(w.pose));
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
            const int32_t result = w.api.configure(w.status.kernel_session, current_fence,
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

            smf_camera_input input = w.next_input(current_fence, seconds);
            // The trajectory arrives in the same publication as the settings, so the two stay consistent.
            input.trajectory = w.main.trajectory;
            input.pointer_x = pointer_valid ? x : 0;
            input.pointer_y = pointer_valid ? y : 0;

            if (blocked) input.flags |= SMF_CAMERA_MOTION_BLOCKED;
            if (!keyboard_blocked) {
                input.pan_x = pair_held(w.keys, 6) ? 1 : pair_held(w.keys, 4) ? -1 : 0;
                input.pan_z = pair_held(w.keys, 2) ? -1 : pair_held(w.keys, 0) ? 1 : 0;
            }
            if (!blocked && camera_control::held(XK_Shift_L)) input.flags |= SMF_CAMERA_FAST_PAN;
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

            const int32_t result = w.api.step(w.status.kernel_session, &input, sizeof(input), &w.pose, sizeof(w.pose));
            if (result != SMF_CAMERA_OK) return w.fault(result);
            if (!valid_pose(w.pose)) return w.fault(E_INVALIDARG);

            w.status.steps++;
            w.status.step_qpc = qpc;
            w.status.state = blocked ? bridge_blocked : bridge_owned;
            w.status.flags = (keyboard_blocked ? 1u : 0u) | (blocked ? 2u : 0u) | (middle ? 4u : 0u);
        } else {
            // Until main acknowledges the seed only the kernel clock advances: no input, no
            // trajectory, no clamping, so the seed pose cannot move before main has applied it.
            smf_camera_input idle = w.next_input(current_fence, seconds);
            idle.flags = SMF_CAMERA_CLOCK_ONLY;

            const int32_t result = w.api.step(w.status.kernel_session, &idle, sizeof(idle), &w.pose, sizeof(w.pose));
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

        w.prepared = desired_from(w.pose);
        w.prepared_valid = true;
        w.prepared_publish = true;
        target = w.prepared;
        return S_OK;
    }

    void worker_committed(HRESULT result) {
        worker_state& w = worker;
        if (native_thread() != w.status.worker_thread) return;

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
        if (native_thread() != w.status.worker_thread) return;

        if (w.prepared_valid && w.prepared.epoch == fence.load(std::memory_order_acquire)) {
            w.status.desired = w.prepared_publish ? w.prepared : smf_bridge_desired{};
        }

        w.prepared_valid = false;
        w.prepared_publish = false;
        w.report();
    }

    void worker_removed() {
        worker_state& w = worker;
        if (native_thread() != w.status.worker_thread) return;

        camera_control::clear();
        if (w.status.kernel_session && w.api.release) {
            smf_camera_pose output{};
            const int32_t result = w.api.release(w.status.kernel_session, &output, sizeof(output));
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

SMF_BRIDGE_API double smf_camera_bridge_now() {
    return double(camera_bridge::now()) / 1000000000.0;
}

SMF_BRIDGE_API int32_t smf_camera_bridge_init(const char* path, uint32_t characters) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!path || characters < 2 || characters >= 1024 || path[0] != '/') return E_INVALIDARG;
    for (uint32_t i = 0; i < characters; ++i) {
        if (!path[i]) return E_INVALIDARG;
    }

    try_lock lock(path_gate);
    if (!lock) return S_FALSE;

    if (kernel_characters) {
        const bool same = kernel_characters == characters && std::memcmp(kernel_path, path, characters) == 0;
        return same ? S_OK : E_INVALIDARG;
    }

    std::memcpy(kernel_path, path, characters);
    kernel_path[characters] = 0;
    kernel_characters = characters;
    return S_OK;
}

SMF_BRIDGE_API int32_t smf_camera_bridge_publish(const smf_bridge_main* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->size != sizeof(*value) || value->version != 2 || !value->publication) {
        return E_INVALIDARG;
    }

    const smf_bridge_main_state& state = value->state;
    const smf_camera_settings& settings = value->settings;
    const smf_bridge_bindings& bindings = value->bindings;

    if (state.version != 2 || state.size != sizeof(smf_bridge_main_state) || state.reserved || (state.flags & ~63u)) {
        return E_INVALIDARG;
    }
    if (settings.version != 2 || settings.size != sizeof(smf_camera_settings) || !settings.revision ||
        settings.reserved || (settings.flags & ~15u)) {
        return E_INVALIDARG;
    }
    if (state.map_id >= 0 && (!is_finite(state.projection_half_height) || state.projection_half_height <= 0)) {
        return E_INVALIDARG;
    }
    if (!valid_trajectory(value->trajectory)) return E_INVALIDARG;
    if (bindings.version != 1 || bindings.size != sizeof(smf_bridge_bindings) || !bindings.revision) return E_INVALIDARG;
    if (state.epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;

    try_lock lock(incoming_gate);
    if (!lock) return S_FALSE;
    if (has_incoming && value->publication <= incoming.publication) return E_INVALIDARG;

    incoming = *value;
    has_incoming = true;
    return S_OK;
}

SMF_BRIDGE_API int32_t smf_camera_bridge_revoke(uint64_t epoch, uint32_t reason) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!epoch || epoch <= fence.load(std::memory_order_acquire) || reason > 10) return E_INVALIDARG;

    fence.store(epoch, std::memory_order_release);
    return S_OK;
}

SMF_BRIDGE_API int32_t smf_camera_bridge_desired(smf_bridge_desired* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;
    *value = {};

    try_lock lock(outgoing_gate);
    if (!lock) return S_FALSE;
    if (outgoing.state == bridge_fault && fault_epoch.load(std::memory_order_acquire) == fence.load(std::memory_order_acquire)) {
        return E_FAIL;
    }
    if (!outgoing.desired.sequence || outgoing.desired.epoch != fence.load(std::memory_order_acquire)) return S_FALSE;

    *value = outgoing.desired;
    return S_OK;
}

SMF_BRIDGE_API int32_t smf_camera_bridge_status(smf_bridge_status* value, uint32_t bytes) {
    using namespace camera_bridge;
    // Diagnostics may read this from any thread while Unity is blocked, so it must not claim main.
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;

    try_lock lock(outgoing_gate);
    if (!lock) return S_FALSE;

    *value = outgoing;
    value->fence_epoch = fence.load(std::memory_order_acquire);
    return S_OK;
}

SMF_CONTROL_API smf_camera_control_policy(const smf_control_policy* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;
    return camera_control::publish_policy(*value);
}

SMF_CONTROL_API smf_camera_control_impulse(const smf_control_impulse* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value) || value->epoch != fence.load(std::memory_order_acquire)) return E_INVALIDARG;
    return camera_control::queue(*value);
}

SMF_CONTROL_API smf_camera_control_status(smf_control_status* value, uint32_t bytes) {
    using namespace camera_bridge;
    if (FAILED(on_main_thread())) return E_UNEXPECTED;
    if (!value || bytes != sizeof(*value)) return E_INVALIDARG;
    return camera_control::read_status(*value);
}
