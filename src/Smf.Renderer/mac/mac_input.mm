#include "platform.h"
#include "../common/wheel_modifiers.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace camera_control {
    namespace {

        // smf_control_wheel_status::state values.
        constexpr uint32_t tap_starting = 1;
        constexpr uint32_t tap_running = 2;
        constexpr uint32_t tap_stopped = 3;
        constexpr uint32_t tap_failed = 4;

        // smf_control_policy::flags bits.
        constexpr uint32_t policy_edge_eligible = 1;
        constexpr uint32_t policy_fullscreen = 2;
        constexpr uint32_t policy_wheel_unsafe = 4;
        constexpr uint32_t policy_camera_owned = 8;

        // Window origin, size and pixel scale of the game view in screen points.
        struct geometry_state {
            double x = 0;
            double y = 0;
            double width = 0;
            double height = 0;
            double scale_x = 1;
            double scale_y = 1;
        };

        struct wheel_event {
            uint64_t timestamp = 0;
            uint64_t epoch = 0;
            uint64_t focus_epoch = 0;
            double x = 0;
            double y = 0;
            double delta = 0;
            uint32_t modifiers = 0;
        };

        // The gate covers geometry and policy; main writes, the compositor copies.
        std::mutex& gate = *new std::mutex();
        geometry_state geometry{};
        geometry_state worker_geometry{};
        smf_control_policy policy{};
        smf_control_policy worker_policy{};

        // Key impulses: main queues, the compositor drains.
        smf_control_impulse impulses[128]{};
        uint64_t last_sequence = 0;
        std::atomic<uint64_t> policy_revision{0};
        std::atomic<uint64_t> head{0};
        std::atomic<uint64_t> tail{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> consumed{0};
        std::atomic<uint64_t> stale{0};
        std::atomic<uint64_t> blocked{0};
        std::atomic<uint64_t> full{0};
        std::atomic<uint64_t> busy{0};
        std::atomic<uint32_t> high_water{0};

        // Wheel events: the tap thread queues, the compositor drains.
        wheel_event wheels[1024]{};
        std::atomic<uint64_t> wheel_head{0};
        std::atomic<uint64_t> wheel_tail{0};
        std::atomic<uint64_t> observed{0};
        std::atomic<uint64_t> wheel_consumed{0};
        std::atomic<uint64_t> denied{0};
        std::atomic<uint64_t> wheel_stale{0};
        std::atomic<uint64_t> overflow{0};

        std::atomic<uint64_t> window{0};
        std::atomic<uint64_t> active_epoch{0};
        std::atomic<uint64_t> focus_epoch{0};
        std::atomic<uint64_t> focus_since{0};
        std::atomic<uint32_t> tap_state{0};
        std::atomic<uint32_t> tap_thread{0};
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> done{false};
        std::atomic<bool> focused{false};
        std::atomic<uint32_t> wheel_reservations{0};
        std::thread& tap = *new std::thread();

        // Compositor-only key state. Keys already down when focus arrives stay blocked until released.
        bool keyboard_allowed = false;
        uint64_t key_epoch = 0;
        uint64_t seen_focus = 0;
        uint64_t key_seen[128]{};
        bool key_blocked[128]{};

        bool policy_allows(const smf_control_policy& p, uint64_t epoch, double x, double y) {
            if (p.epoch != epoch || (p.flags & (policy_wheel_unsafe | policy_camera_owned)) != policy_camera_owned ||
                x < 0 || y < 0 || x >= p.width || y >= p.height) return false;

            for (uint32_t i = 0; i < p.rect_count; ++i) {
                const auto& r = p.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return false;
            }
            return true;
        }

        long dictionary_number(CFDictionaryRef entry, CFStringRef key) {
            CFTypeRef value = CFDictionaryGetValue(entry, key);
            long result = -1;
            if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)value, kCFNumberLongType, &result);
            return result;
        }

        bool entirely_offscreen(CFDictionaryRef entry) {
            auto value = CFDictionaryGetValue(entry, kCGWindowBounds);
            CGRect bounds{};
            if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID() ||
                !CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)value, &bounds) ||
                !std::isfinite(bounds.origin.x) || !std::isfinite(bounds.origin.y) ||
                !std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height)) return false;

            uint32_t displays = 0;
            return CGGetDisplaysWithRect(bounds, 0, nullptr, &displays) == kCGErrorSuccess && displays == 0;
        }

        // True while the game is the front process and its window is the topmost visible one.
        bool refresh_focus() {
            ProcessSerialNumber psn{};
            pid_t pid = 0;
            OSStatus code = GetFrontProcess(&psn);
            if (!code) code = GetProcessPID(&psn, &pid);

            bool own = false;
            uint64_t first = 0;
            long owner = -1;
            CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);

            if (windows) {
                for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
                    auto item = (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
                    auto number = uint64_t(dictionary_number(item, kCGWindowNumber));
                    auto process = dictionary_number(item, kCGWindowOwnerPID);

                    // Fullscreen AppKit title bars can stay in the on-screen list at
                    // y = -height. They cover no display and must not steal camera focus.
                    // Unknown bounds and every actually visible window keep the guard.
                    if (!first && dictionary_number(item, kCGWindowLayer) == 0 && !entirely_offscreen(item)) {
                        first = number;
                        owner = process;
                    }
                    if (number == window.load() && process == getpid()) own = true;
                }

                CFRelease(windows);
            }

            bool next = !code && pid == getpid() && own && first == window.load() && owner == getpid();
            if (focused.exchange(next) != next) {
                focus_since = native_now();
                focus_epoch.fetch_add(1);
                active_epoch = 0;
            }
            return next;
        }

        CGEventRef wheel_tap_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
            if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
                tap_state = tap_failed;
                active_epoch = 0;
                return event;
            }

            if (type != kCGEventScrollWheel || !event || stop_requested.load()) return event;

            ++observed;
            int64_t target = CGEventGetIntegerValueField(event, kCGEventTargetUnixProcessID);
            auto flags = CGEventGetFlags(event);
            uint32_t modifiers = wheel_modifiers((flags & kCGEventFlagMaskControl) != 0,
                (flags & kCGEventFlagMaskAlternate) != 0, (flags & kCGEventFlagMaskShift) != 0);
            uint64_t epoch = active_epoch.load();

            if ((target > 0 && target != getpid()) || !focused.load() || !epoch ||
                !wheel_modifiers_allowed(wheel_reservations.load(), modifiers)) {
                ++denied;
                return event;
            }

            CGPoint p = CGEventGetLocation(event);
            uint64_t end = wheel_head.load();
            uint64_t first = wheel_tail.load(std::memory_order_acquire);

            if (end - first >= 1024) {
                ++overflow;
                tap_state = tap_failed;
                active_epoch = 0;
                return event;
            }

            // Match Unity's NSEvent to IMGUI conversion. Fixed-point CG deltas use a
            // different scale; precise mice and trackpads need Unity's pixel normalization.
            double delta = 0;
            @autoreleasepool {
                @try {
                    NSEvent* native = [NSEvent eventWithCGEvent:event];
                    if (!native || native.type != NSEventTypeScrollWheel) {
                        ++denied;
                        return event;
                    }

                    float value = -float(native.scrollingDeltaY);
                    if (native.hasPreciseScrollingDeltas) value /= 20.0f;
                    delta = value;
                } @catch (NSException*) {
                    ++denied;
                    tap_state = tap_failed;
                    active_epoch = 0;
                    return event;
                }
            }

            if (!std::isfinite(delta) || std::abs(delta) > 10000) {
                ++denied;
                return event;
            }

            wheels[end % 1024] = {CGEventGetTimestamp(event), epoch, focus_epoch.load(), p.x, p.y, delta, modifiers};
            wheel_head.store(end + 1, std::memory_order_release);
            return event; // Listen only: never consume, alter, repost or synthesize Unity input.
        }

        void run_tap() {
            @autoreleasepool {
                tap_thread = native_thread();

                auto port = CGEventTapCreateForPid(getpid(), kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
                    CGEventMaskBit(kCGEventScrollWheel), wheel_tap_callback, nullptr);
                auto source = port ? CFMachPortCreateRunLoopSource(nullptr, port, 0) : nullptr;

                if (port && source && CGEventTapIsEnabled(port)) {
                    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
                    tap_state = tap_running;

                    while (!stop_requested.load()) {
                        @autoreleasepool {
                            refresh_focus();
                            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .008, true);
                        }
                    }

                    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
                } else {
                    tap_state = tap_failed;
                }

                active_epoch = 0;
                focused = false;

                if (port) {
                    CGEventTapEnable(port, false);
                    CFMachPortInvalidate(port);
                    CFRelease(port);
                }
                if (source) CFRelease(source);
                if (tap_state != tap_failed) tap_state = tap_stopped;

                done.store(true, std::memory_order_release);
            }
        }

    }

    void publish_geometry(double x, double y, double width, double height, double scale_x, double scale_y) {
        if (!pthread_main_np() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
            !std::isfinite(scale_x) || !std::isfinite(scale_y) || width <= 0 || height <= 0 || scale_x <= 0 || scale_y <= 0) return;

        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (lock) geometry = {x, y, width, height, scale_x, scale_y};
    }

    int start(uint64_t window_number) {
        if (!pthread_main_np() || !window_number || tap.joinable()) return E_UNEXPECTED;

        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (!lock) return S_FALSE;

        // The previous compositor and tap have both joined before a new session starts.
        policy = {};
        worker_policy = {};
        policy_revision = 0;
        last_sequence = 0;

        head = 0;
        tail = 0;
        wheel_head = 0;
        wheel_tail = 0;
        accepted = 0;
        consumed = 0;
        stale = 0;
        blocked = 0;
        full = 0;
        busy = 0;
        observed = 0;
        wheel_consumed = 0;
        denied = 0;
        wheel_stale = 0;
        overflow = 0;
        high_water = 0;

        window = window_number;
        active_epoch = 0;
        focused = false;
        focus_since = native_now();
        focus_epoch.fetch_add(1);
        stop_requested = false;
        done = false;
        tap_state = tap_starting;

        tap = std::thread(run_tap);
        return S_OK;
    }

    void stop() {
        stop_requested.store(true);
        active_epoch = 0;
    }

    bool join() {
        if (!pthread_main_np() || !done.load(std::memory_order_acquire)) return false;
        if (tap.joinable()) tap.join();
        return true;
    }

    bool observe(double& x, double& y, bool& middle, bool& focus, bool& left) {
        {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (lock) {
                worker_geometry = geometry;
                worker_policy = policy;
            }
        }

        focus = refresh_focus() && tap_state.load() == tap_running;
        CGEventRef location = CGEventCreate(nullptr);
        if (!location) {
            focus = false;
            return false;
        }

        CGPoint p = CGEventGetLocation(location);
        CFRelease(location);
        x = (p.x - worker_geometry.x) * worker_geometry.scale_x;
        y = (p.y - worker_geometry.y) * worker_geometry.scale_y;
        middle = focus && CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonCenter);
        left = focus && CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);

        if (!focus) {
            keyboard_focus(false);
            active_epoch = 0;
        }

        return focus && worker_geometry.width > 0 && worker_geometry.height > 0;
    }

    void keyboard_focus(bool camera_keys_allowed) {
        uint64_t epoch = focus_epoch.load();
        if (camera_keys_allowed != keyboard_allowed || epoch != seen_focus) {
            ++key_epoch;
            seen_focus = epoch;
            keyboard_allowed = camera_keys_allowed;
        }
    }

    bool held(int quartz_key) {
        if (!keyboard_allowed || !focused.load() || quartz_key <= 0 || quartz_key > 128) return false;

        uint32_t key = uint32_t(quartz_key - 1);
        bool down = CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, CGKeyCode(key));
        if (key_seen[key] != key_epoch) {
            key_seen[key] = key_epoch;
            key_blocked[key] = down;
        }

        if (!down) key_blocked[key] = false;
        return down && !key_blocked[key];
    }

    HRESULT publish_policy(const smf_control_policy& p) {
        if (!pthread_main_np() || p.size != 1088 || p.version != 1 || !p.epoch || !p.revision || p.rect_count > 64 ||
            (p.flags & ~127u) || !std::isfinite(p.ui_scale) || p.ui_scale <= 0 ||
            !std::isfinite(p.inspect_height) || p.inspect_height < 0) return E_INVALIDARG;

        for (uint32_t i = 0; i < p.rect_count; ++i) {
            const auto& r = p.rects[i];
            if (!std::isfinite(r.left) || !std::isfinite(r.top) || !std::isfinite(r.right) || !std::isfinite(r.bottom) ||
                r.left > r.right || r.top > r.bottom) return E_INVALIDARG;
        }

        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
        if (!lock) {
            ++busy;
            return S_FALSE;
        }

        if (p.revision <= policy.revision) return E_INVALIDARG;
        policy = p;
        wheel_reservations.store(p.flags);
        policy_revision = p.revision;
        return S_OK;
    }

    HRESULT queue(const smf_control_impulse& p) {
        if (!pthread_main_np() || p.size != 48 || p.version != 1 || !p.epoch || p.sequence <= last_sequence || p.reserved ||
            (p.flags & ~3u) || !p.flags || p.wheel_delta != 0) return E_INVALIDARG;

        uint64_t end = head.load();
        uint64_t first = tail.load(std::memory_order_acquire);
        if (end - first >= 128) {
            ++full;
            return E_FAIL;
        }

        impulses[end % 128] = p;
        last_sequence = p.sequence;
        high_water.store(std::max(high_water.load(), uint32_t(end - first + 1)));
        ++accepted;
        head.store(end + 1, std::memory_order_release);
        return S_OK;
    }

    void prepare(uint64_t epoch, bool motion_blocked, double x, double y, smf_camera_input& input) {
        if (worker_policy.epoch == epoch) {
            if (worker_policy.flags & policy_edge_eligible) input.flags |= SMF_CAMERA_ALLOW_EDGE_SCROLL;
            if (worker_policy.flags & policy_fullscreen) input.flags |= SMF_CAMERA_FULLSCREEN;
            input.inspect_pane_height = worker_policy.inspect_height;
            for (uint32_t i = 0; i < worker_policy.rect_count; ++i) {
                const auto& r = worker_policy.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) input.flags |= SMF_CAMERA_POINTER_OVER_UI;
            }
        }

        // One key impulse per step; stale and blocked ones are dropped.
        uint64_t end = head.load(std::memory_order_acquire);
        uint64_t first = tail.load();

        while (first < end) {
            auto q = impulses[first % 128];
            tail.store(++first, std::memory_order_release);

            if (q.epoch != epoch) {
                ++stale;
                continue;
            }
            if (motion_blocked) {
                ++blocked;
                continue;
            }

            input.flags |= q.flags;
            ++consumed;
            break;
        }

        bool ready = !motion_blocked && focused.load() && tap_state.load() == tap_running && worker_policy.epoch == epoch &&
            (worker_policy.flags & (policy_wheel_unsafe | policy_camera_owned)) == policy_camera_owned;
        active_epoch = ready ? epoch : 0;

        uint64_t stop_at = wheel_head.load(std::memory_order_acquire);
        uint64_t cursor = wheel_tail.load();

        for (uint32_t n = 0; n < 128 && cursor < stop_at; ++n) {
            auto q = wheels[cursor % 1024];
            wheel_tail.store(++cursor, std::memory_order_release);

            if (q.epoch != epoch || q.focus_epoch != focus_epoch.load() || q.timestamp < focus_since.load()) {
                ++wheel_stale;
                continue;
            }
            double px = (q.x - worker_geometry.x) * worker_geometry.scale_x;
            double py = (q.y - worker_geometry.y) * worker_geometry.scale_y;
            if (!ready || !wheel_modifiers_allowed(worker_policy.flags, q.modifiers) || !policy_allows(worker_policy, epoch, px, py)) {
                ++denied;
                continue;
            }

            input.wheel_delta += q.delta;
            ++wheel_consumed;
        }
    }

    void clear() {
        keyboard_focus(false);
        active_epoch = 0;

        auto n = head.load();
        stale.fetch_add(n - tail.load());
        tail = n;

        auto w = wheel_head.load();
        wheel_stale.fetch_add(w - wheel_tail.load());
        wheel_tail = w;
        worker_policy = {};
    }

    HRESULT read_status(smf_control_status& p) {
        p = {sizeof(p), 1, uint32_t(head.load() - tail.load()), high_water.load(), accepted.load(), consumed.load(),
            stale.load(), blocked.load(), full.load(), busy.load(), policy_revision.load()};
        return S_OK;
    }

    HRESULT wheel_status(smf_control_wheel_status& p) {
        const uint32_t state = tap_state.load();
        p = {sizeof(p), 1, state, state == tap_failed ? E_NOTIMPL : 0, window.load(), focus_epoch.load(), observed.load(),
            wheel_head.load() - wheel_tail.load(), wheel_consumed.load(), denied.load(), wheel_stale.load(), overflow.load(),
            active_epoch.load(), tap_thread.load(), 0};
        return S_OK;
    }

}

SMF_MAC_API int32_t smf_camera_control_wheel_status(smf_control_wheel_status* p, uint32_t bytes) {
    if (!p || bytes != sizeof(*p)) return E_INVALIDARG;
    return camera_control::wheel_status(*p);
}
