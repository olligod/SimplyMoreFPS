#pragma once
#include <stdint.h>

namespace present_format {

    // Unity may render into a typeless texture through a typed view while the DXGI
    // buffer itself is typed. Treat those as one family; never cross RGBA and BGRA.
    inline uint32_t family(uint32_t format) {
        if (format == 27 || format == 28 || format == 29) return 27; // RGBA8
        if (format == 87 || format == 90 || format == 91) return 90; // BGRA8
        return format;
    }

    inline bool matches(uint32_t actual, uint32_t marker, bool unity_target) {
        return actual == marker || (unity_target && family(actual) == family(marker));
    }

}
