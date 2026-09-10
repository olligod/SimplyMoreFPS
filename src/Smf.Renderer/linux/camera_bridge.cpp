#include "camera_bridge.h"
#include "../common/camera_worker.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <dlfcn.h>

namespace camera_bridge {
    namespace {

        using namespace camera_worker_protocol;

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
            camera_worker_protocol::worker camera;
            int32_t keys[8]{};

            // Never waits on the mailbox; a busy main side is counted instead.
            void report() {
                camera.status.fence_epoch = fence.load(std::memory_order_acquire);
                camera.status.main_thread = main_thread.load();

                try_lock lock(outgoing_gate);
                if (lock) {
                    outgoing = camera.status;
                } else {
                    camera.status.mailbox_drops++;
                }
            }

            // A camera fault must not make the compositor detach its layers, hence S_FALSE.
            HRESULT fault(int32_t error) {
                fault_epoch.store(camera.have_main ? camera.main.state.epoch : 0, std::memory_order_release);
                camera.fault(error);

                report();
                return S_FALSE;
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

    }

    void worker_ready(uint32_t thread, int64_t frequency) {
        worker.camera.status.worker_thread = thread;
        worker.camera.status.qpc_frequency = frequency;
        worker.camera.status.state = bridge_waiting;
        worker.report();
    }

    HRESULT worker_prepare(bool focused, bool pointer_valid, double x, double y, bool middle, smf_bridge_desired& target) {
        worker_state& w = worker;
        auto& c = w.camera;
        c.begin_prepare();
        if (native_thread() != c.status.worker_thread || c.status.qpc_frequency <= 0) return E_UNEXPECTED;

        {
            try_lock lock(incoming_gate);
            if (lock && has_incoming) c.accept_main(incoming);
        }

        const uint64_t current_fence = fence.load(std::memory_order_acquire);
        if (!c.matches_epoch(current_fence)) {
            c.status.state = bridge_waiting;
            c.withdraw();
            w.report();
            return S_FALSE;
        }

        const smf_bridge_main_state& main = c.main.state;
        c.status.applied_sequence = main.applied_sequence;

        if ((main.flags & flag_eligible) == 0) {
            // Not eligible: hand main's own pose back rather than resetting from the source image.
            c.status.state = bridge_waiting;
            c.withdraw();

            if (c.prepare_main_pose(current_fence, target)) {
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
        if (!resolve_bindings(c.main.bindings, w.keys)) return w.fault(E_NOTIMPL);

        const int64_t qpc = now();
        const double seconds = double(qpc) / c.status.qpc_frequency;

        const int32_t adopted = c.adopt_epoch(w.api, current_fence, seconds, E_INVALIDARG);
        if (adopted != SMF_CAMERA_OK) return w.fault(adopted);

        if (c.status.state == bridge_fault) {
            w.report();
            return S_FALSE;
        }

        const int32_t configured = c.configure(w.api, current_fence, E_INVALIDARG);
        if (configured != SMF_CAMERA_OK) return w.fault(configured);

        if (c.acknowledged()) {
            const bool blocked = !focused || !pointer_valid || (main.flags & flag_motion_blocked) != 0;
            const bool keyboard_blocked = blocked || (main.flags & (flag_text_captured | flag_search_focused)) != 0;

            smf_camera_input input = c.motion_input(current_fence, seconds, blocked, pointer_valid, x, y);
            if (!keyboard_blocked) {
                input.pan_x = pair_held(w.keys, 6) ? 1 : pair_held(w.keys, 4) ? -1 : 0;
                input.pan_z = pair_held(w.keys, 2) ? -1 : pair_held(w.keys, 0) ? 1 : 0;
            }
            if (!blocked && camera_control::held(XK_Shift_L)) input.flags |= SMF_CAMERA_FAST_PAN;
            c.pointer_motion(blocked, x, y, middle, input);

            camera_control::prepare(current_fence, blocked, x, y, input);

            const int32_t result = c.step(w.api, input, qpc, blocked, keyboard_blocked, middle, E_INVALIDARG);
            if (result != SMF_CAMERA_OK) return w.fault(result);
        } else {
            const int32_t result = c.wait_for_seed(w.api, current_fence, seconds, E_INVALIDARG, E_NOTIMPL);
            if (result != SMF_CAMERA_OK) return w.fault(result);
        }

        if (fence.load(std::memory_order_acquire) != current_fence) {
            c.withdraw();
            w.report();
            return S_FALSE;
        }

        c.prepare_pose(target);
        return S_OK;
    }

    void worker_committed(HRESULT result) {
        worker_state& w = worker;
        auto& c = w.camera;
        if (native_thread() != c.status.worker_thread) return;

        if (FAILED(result)) {
            w.fault(result);
            return;
        }

        if (result == S_OK && c.prepared_valid && c.prepared.epoch == fence.load(std::memory_order_acquire)) {
            c.committed(now());
        }

        c.begin_prepare();
        w.report();
    }

    void worker_reused_pose() {
        worker_state& w = worker;
        auto& c = w.camera;
        if (native_thread() != c.status.worker_thread) return;

        if (c.prepared_valid && c.prepared.epoch == fence.load(std::memory_order_acquire)) {
            c.publish_prepared();
        }

        c.begin_prepare();
        w.report();
    }

    void worker_removed() {
        worker_state& w = worker;
        auto& c = w.camera;
        if (native_thread() != c.status.worker_thread) return;

        camera_control::clear();
        c.removed(w.api);
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
