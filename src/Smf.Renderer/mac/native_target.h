#pragma once
#include "source_target.h"

namespace mac {

    // Complete native frames exist before a generation and during restoration, so
    // generation and content are joined to the actual marker instead of inferred.
    inline bool valid_native_target(const mac_source_target& p) {
        return p.size == 64 && p.version == 1 && p.session && p.source_frame && p.native_render_buffer &&
            p.width && p.height && p.width <= 16384 && p.height <= 16384 &&
            uint64_t(p.width) * p.height * 4 <= 64ull * 1024 * 1024 && !p.flags && !p.reserved;
    }

    inline bool native_target_matches(const mac_source_target& p, const session_native_frame& m) {
        return valid_native_target(p) && m.size == 64 && m.version == 1 && !m.flags && !m.reserved &&
            p.session == m.session && p.content_revision == m.content_revision && p.generation == m.generation &&
            p.source_frame == m.source_frame && p.width == m.width && p.height == m.height;
    }

    struct native_target_stage {
        mac_source_target value{};

        void clear() { value = {}; }
        bool matches(const session_native_frame& m) const { return native_target_matches(value, m); }

        int stage(const mac_source_target& p, uint64_t queued_frame) {
            if (!valid_native_target(p)) return -201;
            if (p.source_frame <= queued_frame || (value.session == p.session && p.source_frame < value.source_frame)) return 1;
            if (value.session == p.session && value.source_frame == p.source_frame &&
                (p.content_revision != value.content_revision || p.generation != value.generation ||
                 p.native_render_buffer != value.native_render_buffer || p.width != value.width || p.height != value.height)) return -201;

            value = p;
            return 0;
        }
    };

}
