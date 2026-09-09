#pragma once
#include "linux_session_packets.h"
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace linux_session {

    // First session failure. State fields are copied under the gate; draw facts
    // come from the failing worker call.
    struct failure_diagnostic {
        uint32_t size = 1264;
        uint32_t version = 1;
        uint32_t line = 0;
        uint32_t thread = 0;
        uint64_t publication = 0;
        uint64_t published_ns = 0;
        uint64_t session = 0;
        uint64_t phase = 0;
        int32_t error = 0;
        uint32_t owner = 0;
        uint64_t flags = 0;
        status_packet status{};
        command_packet pending{};
        uint64_t draw[64]{};
        uint64_t protocol[16]{};
    };

    inline bool latch_failure(failure_diagnostic& first, const failure_diagnostic& value) {
        if (first.line || !value.line || !value.session) return false;
        first = value;
        return true;
    }

    inline uint64_t failure_bits(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        return bits;
    }

    static_assert(sizeof(failure_diagnostic) == 1264, "Failure1264");
    static_assert(offsetof(failure_diagnostic, status) == 64 && offsetof(failure_diagnostic, pending) == 536 &&
                  offsetof(failure_diagnostic, draw) == 624 && offsetof(failure_diagnostic, protocol) == 1136, "Failure1264 fields");
    static_assert(std::is_trivially_copyable<failure_diagnostic>::value, "plain failure packet");

}
