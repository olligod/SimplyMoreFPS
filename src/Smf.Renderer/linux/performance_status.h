#pragma once
#include <cstddef>
#include <cstdint>

namespace linux_session {

    enum performance_index {
        allocated_names,
        deleted_names,
        storage_bytes_issued,
        blit_calls,
        blit_bytes_issued,
        cache_blits,
        cache_hits,
        owned_callbacks,
        owned_callback_ns,
        max_owned_callback_ns,
        reused_slots,
        incompatible_purges,
        live_names,
        peak_names,
        no_free_slot,
        copy_failures
    };

    struct performance_status {
        uint32_t size = 160;
        uint32_t version = 1;
        uint64_t session = 0;
        uint64_t publication = 0;
        uint64_t published_ns = 0;
        uint64_t values[16]{};
    };

    static_assert(sizeof(performance_status) == 160 && offsetof(performance_status, values) == 32, "Performance160");

}
