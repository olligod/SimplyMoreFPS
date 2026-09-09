#pragma once
#include "present_model.h"

namespace present_observer {

    // Diagnostic snapshot read through smf_mac_terminal_status; separate from the session packets.
    struct present_status {
        uint32_t size = 192, version = 1;
        uint64_t session = 0, queue = 0, layer = 0;
        uint32_t fault = 0, stopping = 0, buffers = 0, drawables = 0, hooks = 0, in_flight = 0;
        present_counters counters{};
        uint64_t source_failures = 0, class_restore_conflicts = 0, source_thread = 0;
    };
    static_assert(sizeof(present_status) == 192, "Terminal status192");

}
