#pragma once
#include "camera_control.h"
#include "../common/wheel_modifiers.h"
#include <atomic>
#include <cstdint>

namespace camera_control {

    struct wheel_event {
        uint64_t generation;
        uint64_t epoch;
        int32_t delta;
        int32_t x;
        int32_t y;
        uint32_t modifiers = 0;
    };

    // Single producer (the hook thread), single consumer (the compositor worker).
    template <uint32_t Capacity>
    class wheel_queue {
    public:
        bool push(const wheel_event& event) {
            const uint64_t h = head.load(std::memory_order_relaxed);
            if (h - tail.load(std::memory_order_acquire) >= Capacity) return false;
            events[h % Capacity] = event;
            head.store(h + 1, std::memory_order_release);
            return true;
        }

        bool pop(wheel_event& event) {
            const uint64_t t = tail.load(std::memory_order_relaxed);
            if (t == head.load(std::memory_order_acquire)) return false;
            event = events[t % Capacity];
            tail.store(t + 1, std::memory_order_release);
            return true;
        }

        uint64_t count() const {
            const uint64_t t = tail.load(std::memory_order_acquire);
            const uint64_t h = head.load(std::memory_order_acquire);
            return h - t;
        }

    private:
        wheel_event events[Capacity]{};
        std::atomic<uint64_t> head{0};
        std::atomic<uint64_t> tail{0};
    };

    inline bool wheel_policy_allows(const smf_control_policy& policy, uint64_t epoch, bool blocked, int x, int y) {
        if (blocked || !epoch || policy.epoch != epoch || (policy.flags & 12) != 8 || !policy.revision) return false;
        if (!policy.width || !policy.height || policy.rect_count > 64) return false;
        if (x < 0 || y < 0 || uint32_t(x) >= policy.width || uint32_t(y) >= policy.height) return false;

        for (uint32_t i = 0; i < policy.rect_count; i++) {
            const auto& r = policy.rects[i];
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return false;
        }

        return true;
    }

    inline bool wheel_event_matches(const wheel_event& event, uint64_t generation, uint64_t epoch) {
        return event.generation == generation && event.epoch == epoch;
    }

    inline double wheel_unity_delta(int32_t os_delta) {
        return -double(os_delta) / 40.0;
    }

}
