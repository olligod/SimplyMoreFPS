#pragma once
#include "linux_session_packets.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace linux_session {

    inline bool preparation_superseded(bool preparing, uint64_t serial, uint64_t content, uint64_t fence) {
        return preparing && serial && content < fence;
    }

    // One published packet plus its publication number and time stamp. GPU owners
    // use try_publish so a reader can never stall a copy or a present.
    template<class Packet>
    class snapshot {
        std::mutex gate;
        Packet value{};
        uint64_t publication = 0;
        int64_t clock = 0;
        bool valid = false;

    public:
        void publish_boundary(const Packet& packet, int64_t now) {
            std::lock_guard<std::mutex> lock(gate);
            value = packet;
            clock = now;
            publication++;
            valid = true;
        }

        bool try_publish(const Packet& packet, int64_t now) {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock) return false;

            value = packet;
            clock = now;
            publication++;
            valid = true;
            return true;
        }

        bool read(Packet& packet, uint64_t& sequence, int64_t& published_at) {
            std::lock_guard<std::mutex> lock(gate);
            if (!valid) return false;

            packet = value;
            sequence = publication;
            published_at = clock;
            return true;
        }
    };

    // The serial/mode word is the activation commit point. Commit is claimed only
    // after the worker swap has been presented; cancel changes the same word.
    class activation_fence {
        std::atomic<uint64_t> stamp{0};
        static constexpr uint64_t limit = UINT64_MAX >> 2;

    public:
        bool arm(uint64_t serial) {
            if (!serial || serial > limit) return false;
            stamp.store((serial << 2) | 1, std::memory_order_release);
            return true;
        }

        bool cancel(uint64_t serial) {
            if (!serial || serial > limit) return false;
            stamp.store((serial << 2) | 3, std::memory_order_release);
            return true;
        }

        bool commit(uint64_t serial) {
            uint64_t expected = (serial << 2) | 1;
            return stamp.compare_exchange_strong(expected, (serial << 2) | 2, std::memory_order_acq_rel);
        }

        bool cancelled() const {
            return (stamp.load(std::memory_order_acquire) & 3) == 3;
        }

        void reset() {
            stamp = 0;
        }
    };

    // Holds one accepted command until a gate owner consumes it. A busy inbox
    // leaves the request unaccepted so the caller retries.
    class command_inbox {
        std::mutex gate;
        command_packet value{};
        bool queued = false;
        uint64_t accepted_serial = 0;

    public:
        activation_fence activation;

        int try_accept(const command_packet& command) {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock || queued) return 1;
            if (!command.serial || command.serial <= accepted_serial || command.serial > (UINT64_MAX >> 2)) return -201;
            if (command.operation == 3 && !activation.arm(command.serial)) return -201;
            if (command.operation == 6 && !activation.cancel(command.serial)) return -201;

            value = command;
            queued = true;
            accepted_serial = command.serial;
            return 0;
        }

        bool try_consume(command_packet& command) {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock || !queued) return false;

            command = value;
            queued = false;
            return true;
        }

        bool empty() {
            std::lock_guard<std::mutex> lock(gate);
            return !queued;
        }

        bool reset() {
            std::lock_guard<std::mutex> lock(gate);
            if (queued) return false;

            accepted_serial = 0;
            activation.reset();
            return true;
        }
    };

}
