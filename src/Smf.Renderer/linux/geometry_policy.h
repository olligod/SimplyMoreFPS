#pragma once
#include <cstdint>

namespace linux_session {

    enum class geometry_outcome { ready, stale, failed };

    struct geometry_ticket {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    inline geometry_outcome classify_geometry(bool query_succeeded, uint64_t x_errors, int map_state, int width, int height,
                                              geometry_ticket expected = {}) {
        if (!query_succeeded || x_errors) return geometry_outcome::failed;
        // X IsViewable is 2.
        if (map_state != 2 || width <= 0 || height <= 0) return geometry_outcome::stale;
        if (width > 16384 || height > 16384) return geometry_outcome::failed;
        if (expected.width && uint32_t(width) != expected.width) return geometry_outcome::stale;
        if (expected.height && uint32_t(height) != expected.height) return geometry_outcome::stale;
        return geometry_outcome::ready;
    }

}
