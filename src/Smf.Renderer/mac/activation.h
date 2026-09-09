#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace mac {

    // Initial activation needs an observed native frame; a replacement inherits the
    // already displayed overlay. Neither counts as evidence for a native restore.
    inline bool activation_ready(bool native_available, bool overlay_visible, uint64_t active_generation, bool restoring) {
        return !restoring && (native_available || (overlay_visible && active_generation));
    }

    // Detach drains the current GPU submission before main hides the layer. Restore
    // and AwaitNative keep drawing the retained composite until Detach is requested.
    inline bool worker_may_draw(bool idle, bool healthy, bool detaching) {
        return idle && healthy && !detaching;
    }

    // Do not consume the first candidate drawable before the visible Show epoch and
    // its attempt exist. A replacement keeps drawing the already active generation.
    inline uint64_t activation_draw_generation(uint64_t active, uint64_t candidate, bool observed, bool attempt_ready) {
        return observed && attempt_ready ? candidate : active;
    }

    // One immutable activation ticket and show epoch. Delayed callbacks retain this
    // object rather than mutable session state, so replacing the attempt isolates them.
    class activation_attempt {
        const uint64_t session, operation, generation, content, shown_ns;
        std::atomic<uint64_t> frame_{0};

    public:
        activation_attempt(uint64_t s, uint64_t op, uint64_t g, uint64_t c, uint64_t shown)
            : session(s), operation(op), generation(g), content(c), shown_ns(shown) {}

        bool matches(uint64_t s, uint64_t op, uint64_t g, uint64_t c, uint64_t shown) const {
            return session == s && operation == op && generation == g && content == c && shown_ns == shown;
        }

        void publish(uint64_t f, uint64_t g, uint64_t c, uint64_t acquired) {
            if (!session || !operation || !generation || !shown_ns || !f || g != generation || c != content || acquired <= shown_ns) return;
            uint64_t empty = 0;
            frame_.compare_exchange_strong(empty, f, std::memory_order_release, std::memory_order_relaxed);
        }

        uint64_t frame() const { return frame_.load(std::memory_order_acquire); }
    };

    // Bound before the callbacks are registered. The RMW join guarantees that
    // whichever callback arrives second sees the first; two plain flag loads can miss.
    class activation_candidate {
        std::shared_ptr<activation_attempt> attempt;
        uint64_t frame = 0, generation = 0, content = 0, acquired = 0;
        std::atomic<unsigned> events{0};

        void signal(unsigned event) {
            const unsigned joined = events.fetch_or(event, std::memory_order_acq_rel) | event;
            if (joined == 3 && attempt) attempt->publish(frame, generation, content, acquired);
        }

    public:
        void bind(std::shared_ptr<activation_attempt> value, uint64_t f, uint64_t g, uint64_t c, uint64_t ns) {
            attempt = std::move(value);
            frame = f;
            generation = g;
            content = c;
            acquired = ns;
        }

        void gpu_completed(bool good) { signal(good ? 1u : 4u); }
        void presented(bool positive) { signal(positive ? 2u : 4u); }
    };

}
