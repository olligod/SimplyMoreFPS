#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mac {

    // A render callback that cannot take the session gate is still a one-shot
    // dispatch. Record its monotonically issued token without racing packet reuse.
    template<size_t N> struct missed_callbacks {
        std::array<std::atomic<int32_t>, N> tokens{};

        void record(size_t index, int32_t token) {
            if (index >= N || token <= 0) return;
            int32_t prior = tokens[index].load(std::memory_order_relaxed);
            while (prior < token && !tokens[index].compare_exchange_weak(prior, token, std::memory_order_release)) {
            }
        }

        bool contains(size_t index, int32_t token) const {
            return index < N && token > 0 && tokens[index].load(std::memory_order_acquire) == token;
        }
    };

    struct texture_budget {
        static constexpr uint64_t limit = 512ull * 1024 * 1024;

        struct item {
            uint64_t identity = 0, bytes = 0;
        };

        std::array<item, 26> items{};
        size_t count = 0;
        uint64_t bytes = 0;
        bool valid = true;

        void add(uint64_t identity, uint32_t w, uint32_t h) {
            if (!identity) return;

            uint64_t size = uint64_t(w) * h * 4;
            for (size_t i = 0; i < count; ++i) {
                if (items[i].identity == identity) {
                    if (items[i].bytes != size) valid = false;
                    return;
                }
            }

            if (count == items.size() || !w || !h || w > 16384 || h > 16384) {
                valid = false;
                return;
            }

            items[count++] = {identity, size};
            bytes += size;
        }

        bool allows(uint64_t extra, uint64_t display_reserve) const {
            return valid && bytes <= limit && display_reserve <= limit - bytes && extra <= limit - bytes - display_reserve;
        }
    };

    inline bool source_may_retire(bool completed, bool visible, bool in_flight) {
        return completed && !visible && !in_flight;
    }

    inline bool acquired_after_show(bool visible, uint64_t acquired_ns, uint64_t show_completed_ns) {
        return visible && show_completed_ns && acquired_ns > show_completed_ns;
    }

    inline bool original_behind_overlay(uint64_t expected_session, uint64_t presented_session, uint64_t acquired_ns, uint64_t show_completed_ns) {
        return expected_session && expected_session == presented_session && acquired_after_show(true, acquired_ns, show_completed_ns);
    }

}
