#pragma once
#include "session_packets.h"

// The handle comes from Graphics.activeColorBuffer on Unity main and is only
// resolved inside its matching render event. It is not an MTLTexture pointer.
#pragma pack(push, 8)
struct mac_source_target {
    uint32_t size, version;
    uint64_t session, content_revision, generation, source_frame, native_render_buffer;
    uint32_t width, height, flags, reserved; // v1 flags must be zero
};
#pragma pack(pop)

static_assert(sizeof(mac_source_target) == 64, "MacTarget64");
static_assert(offsetof(mac_source_target, session) == 8, "MacTarget session8");
static_assert(offsetof(mac_source_target, native_render_buffer) == 40, "MacTarget buffer40");
static_assert(offsetof(mac_source_target, width) == 48, "MacTarget width48");
static_assert(offsetof(mac_source_target, reserved) == 60, "MacTarget reserved60");

namespace mac {

    inline bool valid_source_target(const mac_source_target& value) {
        return value.size == 64 && value.version == 1 && value.session && value.content_revision &&
            value.generation && value.source_frame && value.native_render_buffer && value.width && value.height &&
            value.width <= 16384 && value.height <= 16384 && uint64_t(value.width) * value.height * 4 <= 64ull * 1024 * 1024 &&
            !value.flags && !value.reserved;
    }

    inline bool source_target_matches(const mac_source_target& target, const session_pre_gui& pre) {
        return valid_source_target(target) && pre.size == 64 && pre.version == 1 && !pre.reserved && !(pre.flags & ~1u) &&
            target.session == pre.session && target.content_revision == pre.content_revision &&
            target.generation == pre.generation && target.source_frame == pre.source_frame &&
            target.width == pre.width && target.height == pre.height;
    }

    // Plain data only. A busy ticket queue does not consume the stage, and a newer
    // frame of the current generation replaces it without retaining any engine object.
    struct source_target_stage {
        mac_source_target value{};

        void clear() { value = {}; }
        bool matches(const session_pre_gui& pre) const { return source_target_matches(value, pre); }

        int stage(const mac_source_target& next, uint64_t accepted_frame) {
            if (!valid_source_target(next)) return -201;
            if (next.source_frame <= accepted_frame) return 1;

            if (value.session == next.session && value.content_revision == next.content_revision && value.generation == next.generation) {
                if (next.source_frame < value.source_frame) return 1;
                if (next.source_frame == value.source_frame &&
                    (next.native_render_buffer != value.native_render_buffer || next.width != value.width || next.height != value.height)) return -201;
            }

            value = next;
            return 0;
        }
    };

}
