#pragma once
#include "session_transport.h"
#include <cstddef>

namespace linux_session {

    enum swap_route {
        route_other_target,
        route_original,
        route_hidden_source_owner,
        route_hidden_same_context_other_thread,
        route_hidden_other_context,
        swap_route_count
    };

    struct swap_sample {
        uint64_t words[16]{};
    };

    struct swap_trace_packet {
        uint32_t size = 792;
        uint32_t version = 1;
        uint64_t session = 0;
        uint64_t read_ns = 0;
        uint64_t configured_display = 0;
        uint64_t entered[swap_route_count]{};
        uint64_t returned[swap_route_count]{};
        uint64_t sample_drops[swap_route_count]{};
        swap_sample last[swap_route_count]{};
    };

    static_assert(sizeof(swap_trace_packet) == 792 && offsetof(swap_trace_packet, last) == 152, "SwapTrace792");

    inline swap_route classify_swap_route(bool target_matches, bool hidden, bool source_owner, bool same_context) {
        if (!target_matches) return route_other_target;
        if (!hidden) return route_original;
        if (source_owner) return route_hidden_source_owner;
        if (same_context) return route_hidden_same_context_other_thread;
        return route_hidden_other_context;
    }

    class swap_trace {
        std::atomic<uint64_t> session{0};
        std::atomic<uint64_t> configured_display{0};
        std::atomic<uint64_t> entered[swap_route_count]{};
        std::atomic<uint64_t> returned[swap_route_count]{};
        std::atomic<uint64_t> drops[swap_route_count]{};
        snapshot<swap_sample> samples[swap_route_count];

    public:
        void reset(uint64_t value) {
            session = 0;
            configured_display = 0;

            for (unsigned i = 0; i < swap_route_count; i++) {
                entered[i] = 0;
                returned[i] = 0;
                drops[i] = 0;
                samples[i].publish_boundary({}, 0);
            }

            session = value;
        }

        void configure(uint64_t value) {
            configured_display = value;
        }

        uint64_t enter(swap_route route) {
            return entered[route].fetch_add(1) + 1;
        }

        void finish(swap_route route, const swap_sample& sample) {
            returned[route].fetch_add(1);
            if (!samples[route].try_publish(sample, int64_t(sample.words[2]))) drops[route].fetch_add(1);
        }

        bool read(swap_trace_packet& out, uint64_t now) {
            out = {};
            out.session = session.load();
            out.read_ns = now;
            out.configured_display = configured_display.load();
            if (!out.session) return false;

            for (unsigned i = 0; i < swap_route_count; i++) {
                uint64_t sequence = 0;
                int64_t stamp = 0;
                samples[i].read(out.last[i], sequence, stamp);
                out.returned[i] = returned[i].load();
                out.entered[i] = entered[i].load();
                out.sample_drops[i] = drops[i].load();
            }

            return session.load() == out.session;
        }
    };

    void reset_swap_trace(uint64_t session);

}
