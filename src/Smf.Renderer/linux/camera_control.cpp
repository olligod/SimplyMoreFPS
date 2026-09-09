#include "camera_bridge.h"
#include "../common/wheel_modifiers.h"
#include <X11/extensions/XInput2.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>

namespace camera_control {
    namespace {

        // Never destroyed: the worker may still lock it while static destructors run.
        std::mutex& gate = *new std::mutex();
        smf_control_policy policy{};
        smf_control_policy worker_policy{};
        smf_control_impulse impulses[128]{};
        std::atomic<uint64_t> head{0};
        std::atomic<uint64_t> tail{0};
        std::atomic<uint64_t> consumed{0};
        std::atomic<uint64_t> stale{0};
        std::atomic<uint64_t> blocked_count{0};
        std::atomic<uint64_t> full{0};
        std::atomic<uint64_t> busy{0};
        uint64_t last_sequence = 0;
        uint32_t high_water = 0;
        Display* display = nullptr;
        Window window = 0;
        char keys[32]{};
        int opcode = 0;

        struct wheel_event {
            uint64_t epoch;
            double delta;
            double x;
            double y;
            uint32_t modifiers;
        };

        // Worker-only and never destroyed for the same reason as the gate; observe() caps it at 1024.
        std::deque<wheel_event>& wheels = *new std::deque<wheel_event>();
        std::atomic<uint32_t> wheel_state{0};
        std::atomic<uint32_t> worker_thread{0};
        std::atomic<uint64_t> observed{0};
        std::atomic<uint64_t> wheel_consumed{0};
        std::atomic<uint64_t> denied{0};
        std::atomic<uint64_t> wheel_stale{0};
        std::atomic<uint64_t> overflow{0};
        std::atomic<uint64_t> active_epoch{0};
        std::atomic<uint64_t> source{0};
        std::atomic<uint64_t> queued{0};

        bool allows(const smf_control_policy& p, uint64_t epoch, double x, double y) {
            if (p.epoch != epoch || (p.flags & 12) != 8 || x < 0 || y < 0 || x >= p.width || y >= p.height) return false;
            for (uint32_t i = 0; i < p.rect_count; i++) {
                const smf_control_rect& r = p.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return false;
            }
            return true;
        }

    }

    HRESULT publish_policy(const smf_control_policy& p) {
        if (p.size != 1088 || p.version != 1 || !p.epoch || !p.revision || p.rect_count > 64 || (p.flags & ~127u) ||
            !std::isfinite(p.ui_scale) || p.ui_scale <= 0 || !std::isfinite(p.inspect_height) || p.inspect_height < 0) {
            return E_INVALIDARG;
        }
        for (uint32_t i = 0; i < p.rect_count; i++) {
            const smf_control_rect& r = p.rects[i];
            if (!std::isfinite(r.left) || !std::isfinite(r.right) || !std::isfinite(r.top) || !std::isfinite(r.bottom) ||
                r.right < r.left || r.bottom < r.top) {
                return E_INVALIDARG;
            }
        }

        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (!lock) {
            ++busy;
            return S_FALSE;
        }

        if (p.revision <= policy.revision) return E_INVALIDARG;
        policy = p;
        return S_OK;
    }

    HRESULT queue(const smf_control_impulse& p) {
        if (p.size != 48 || p.version != 1 || !p.epoch || p.sequence <= last_sequence || p.reserved ||
            (p.flags & ~3u) || !p.flags || p.wheel_delta != 0) {
            return E_INVALIDARG;
        }

        const uint64_t end = head.load();
        const uint64_t first = tail.load(std::memory_order_acquire);

        if (end - first >= 128) {
            ++full;
            return E_FAIL;
        }

        impulses[end % 128] = p;
        last_sequence = p.sequence;
        high_water = std::max(high_water, uint32_t(end - first + 1));
        head.store(end + 1, std::memory_order_release);
        return S_OK;
    }

    HRESULT read_status(smf_control_status& p) {
        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (!lock) return S_FALSE;

        const uint64_t end = head.load();
        const uint64_t first = tail.load();
        p = {sizeof(p), 1, uint32_t(end - first), high_water, end, consumed.load(), stale.load(), blocked_count.load(),
            full.load(), busy.load(), policy.revision};
        return S_OK;
    }

    bool held(int key) {
        if (!display || !key) return false;
        const KeyCode code = XKeysymToKeycode(display, KeySym(key));
        return code && (keys[code / 8] & (1 << (code % 8)));
    }

    bool observe(Display* d, Window w, double& x, double& y, bool& middle, bool& focused, bool& left) {
        if (display != d) {
            display = d;
            window = w;
            source.store(w);
            worker_thread.store(native_thread());

            int event = 0;
            int error = 0;
            int major = 2;
            int minor = 0;

            if (!XQueryExtension(d, "XInputExtension", &opcode, &event, &error) || XIQueryVersion(d, &major, &minor) != Success) {
                wheel_state.store(4);
                return false;
            }

            unsigned char mask[XIMaskLen(XI_RawButtonPress)]{};
            XISetMask(mask, XI_RawButtonPress);
            XIEventMask selection{XIAllMasterDevices, int(sizeof(mask)), mask};
            if (XISelectEvents(d, DefaultRootWindow(d), &selection, 1) != Success) {
                wheel_state.store(4);
                return false;
            }

            XFlush(d);
            wheel_state.store(2);
        }

        {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (lock) worker_policy = policy;
        }

        Window focus = 0;
        Window root = 0;
        Window child = 0;
        int revert = 0;
        int rx = 0, ry = 0;
        int px = 0, py = 0;
        unsigned buttons = 0;

        XGetInputFocus(d, &focus, &revert);
        focused = focus == w;
        const bool valid = XQueryPointer(d, w, &root, &child, &rx, &ry, &px, &py, &buttons);
        x = px;
        y = py;
        middle = (buttons & Button2Mask) != 0;
        left = (buttons & Button1Mask) != 0;

        XQueryKeymap(d, keys);

        // Bounded drain of this private connection's own raw XI2 events.
        for (int i = 0; i < 128 && XPending(d); i++) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != GenericEvent || e.xcookie.extension != opcode || !XGetEventData(d, &e.xcookie)) continue;

            if (e.xcookie.evtype == XI_RawButtonPress) {
                auto* raw = static_cast<XIRawEvent*>(e.xcookie.data);
                if (raw->detail == 4 || raw->detail == 5) {
                    ++observed;
                    const uint64_t epoch = active_epoch.load();

                    // Snapshot the modifiers while observing the event; a zoom key may defer the wheel a step.
                    XQueryKeymap(d, keys);
                    const uint32_t modifiers = wheel_modifiers(held(XK_Control_L) || held(XK_Control_R),
                        held(XK_Alt_L) || held(XK_Alt_R), held(XK_Shift_L) || held(XK_Shift_R));

                    if (!valid || !focused || !epoch || !wheel_modifiers_allowed(worker_policy.flags, modifiers) ||
                        !allows(worker_policy, epoch, x, y)) {
                        ++denied;
                    } else if (wheels.size() >= 1024) {
                        ++overflow;
                        wheel_state.store(4);
                        active_epoch.store(0);
                    } else {
                        // Unity's Linux IMGUI reports three delta units per X11 wheel notch (button 4 = -3,
                        // 5 = +3), and the kernel takes IMGUI units.
                        wheels.push_back({epoch, raw->detail == 4 ? -3.0 : 3.0, x, y, modifiers});
                    }
                }
            }

            XFreeEventData(d, &e.xcookie);
        }

        queued.store(wheels.size());
        return valid && wheel_state.load() == 2;
    }

    void prepare(uint64_t epoch, bool blocked, double x, double y, smf_camera_input& p) {
        {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (lock) {
                worker_policy = policy;
            } else {
                ++busy;
            }
        }

        if (worker_policy.epoch == epoch) {
            if (worker_policy.flags & 1) p.flags |= SMF_CAMERA_ALLOW_EDGE_SCROLL;
            if (worker_policy.flags & 2) p.flags |= SMF_CAMERA_FULLSCREEN;
            p.inspect_pane_height = worker_policy.inspect_height;

            for (uint32_t i = 0; i < worker_policy.rect_count; i++) {
                const smf_control_rect& r = worker_policy.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) p.flags |= SMF_CAMERA_POINTER_OVER_UI;
            }
        }

        const uint64_t end = head.load(std::memory_order_acquire);
        uint64_t first = tail.load();

        while (first < end) {
            const smf_control_impulse q = impulses[first % 128];
            tail.store(++first, std::memory_order_release);

            if (q.epoch != epoch) {
                ++stale;
                continue;
            }
            if (blocked) {
                ++blocked_count;
                continue;
            }

            p.flags |= q.flags;
            ++consumed;
            break;
        }

        const bool ready = !blocked && worker_policy.epoch == epoch && (worker_policy.flags & 12) == 8 && wheel_state.load() == 2;
        active_epoch.store(ready ? epoch : 0);

        // A zoom key pulse wins this step; queued wheel events wait for the next one.
        if (p.flags & 3) return;

        while (!wheels.empty()) {
            const wheel_event q = wheels.front();
            wheels.pop_front();

            if (q.epoch != epoch) {
                ++wheel_stale;
                continue;
            }
            if (!ready || !wheel_modifiers_allowed(worker_policy.flags, q.modifiers) || !allows(worker_policy, epoch, q.x, q.y)) {
                ++denied;
                continue;
            }

            p.wheel_delta = q.delta;
            ++wheel_consumed;
            break;
        }

        queued.store(wheels.size());
    }

    void clear() {
        const uint64_t end = head.load();
        stale.fetch_add(end - tail.load());
        tail.store(end);

        wheel_stale.fetch_add(wheels.size());
        wheels.clear();

        queued.store(0);
        active_epoch.store(0);
        worker_policy = {};
        display = nullptr;
        wheel_state.store(3);
    }

    HRESULT wheel_status(smf_control_wheel_status& p) {
        const uint32_t state = wheel_state.load();
        p = {sizeof(p), 1, state, state == 4 ? E_NOTIMPL : 0, source.load(), 1, observed.load(), queued.load(),
            wheel_consumed.load(), denied.load(), wheel_stale.load(), overflow.load(), active_epoch.load(),
            worker_thread.load(), 0};
        return S_OK;
    }

}

SMF_CONTROL_API smf_camera_control_wheel_status(smf_control_wheel_status* p, uint32_t bytes) {
    if (!p || bytes != sizeof(*p)) return E_INVALIDARG;
    return camera_control::wheel_status(*p);
}
