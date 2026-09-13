#pragma once
#include <cstdint>

namespace linux_session {

    constexpr uint64_t scene_source_budget = uint64_t(2) * 1024 * 1024 * 1024;
    constexpr uint64_t scene_packet_budget = scene_source_budget / 2;

    enum class storage_admission { ready, busy, unsupported };

    inline storage_admission scene_capacity(uint64_t used, uint64_t owned, uint64_t required,
                                            uint64_t visible = 0) {
        if (required > scene_packet_budget || visible > scene_source_budget - required || owned > used)
            return storage_admission::unsupported;
        return used - owned <= scene_source_budget - required ? storage_admission::ready : storage_admission::busy;
    }

}
