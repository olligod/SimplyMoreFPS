#pragma once
#include <atomic>
#include <cstdint>

namespace session {

    class scene_memory_budget {
    public:
        explicit scene_memory_budget(uint64_t limit) : maximum(limit) {}

        bool reserve(uint64_t bytes) {
            auto current = allocated.load(std::memory_order_relaxed);
            do {
                if (bytes > maximum || current > maximum - bytes) return false;
            } while (!allocated.compare_exchange_weak(current, current + bytes));
            return true;
        }

        void release(uint64_t bytes) { allocated.fetch_sub(bytes); }
        uint64_t used() const { return allocated.load(); }
        uint64_t limit() const { return maximum; }

    private:
        const uint64_t maximum;
        std::atomic<uint64_t> allocated{0};
    };

}
