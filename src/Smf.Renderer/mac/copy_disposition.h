#pragma once
#include <atomic>

namespace mac {

    // A discarded copy was never submitted; that retires the lease but never
    // certifies GPU completion or usable pixels.
    struct copy_disposition {
        std::atomic<bool> discarded{false};
        bool commit_attempted = false; // render callback only

        void begin_commit() { commit_attempted = true; }

        bool discard() {
            if (commit_attempted) return false;
            discarded.store(true, std::memory_order_release);
            return true;
        }

        bool retired(bool producer_done, bool copy_done) const {
            return producer_done && (copy_done || discarded.load(std::memory_order_acquire));
        }

        bool usable(bool producer_good, bool copy_good) const {
            return producer_good && copy_good && !discarded.load(std::memory_order_acquire);
        }
    };

}
