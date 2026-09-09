#pragma once
#include <cstdint>

namespace mac {

    // Failure-only diagnostic record. No AppKit objects or managed references cross it.
    struct geometry_diagnostic {
        uint32_t size = 96, version = 1;
        int32_t result = 0;
        uint32_t reason = 0;
        uint32_t requested_width = 0, requested_height = 0, refreshed_width = 0, refreshed_height = 0;
        double bounds_width = 0, bounds_height = 0, backing_scale = 0, drawable_width = 0, drawable_height = 0;
        uint64_t source_device = 0, original_device = 0;
        uint32_t flags = 0, reserved = 0;
    };
    static_assert(sizeof(geometry_diagnostic) == 96, "geometry diagnostic ABI");

    // The first failure survives cleanup and repeated reads; only create() resets it.
    inline bool retain_geometry_failure(geometry_diagnostic& first, const geometry_diagnostic& candidate) {
        if (first.result < 0 || candidate.result >= 0) return false;
        first = candidate;
        return true;
    }

}
