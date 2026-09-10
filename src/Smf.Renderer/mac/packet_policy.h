#pragma once
#include "session_packets.h"

namespace mac {

    inline bool preparation_superseded(bool preparing, uint64_t serial, uint64_t content, uint64_t fence) {
        return preparing && serial && content < fence;
    }

    // Called after the main pump: a capture can trigger the first native fault.
    // Native markers keep their own identity and retirement policy during restore.
    inline int capture_queue_owner_result(uint32_t kind, int owner_result) {
        return (kind == 1 || kind == 2) && owner_result < 0 ? owner_result : 0;
    }

    // Both a present and an absent world still carry the validated pose, cache and world GUI texture.
    inline bool valid_world_dispatch(uint32_t flags, uint32_t count) {
        return !(flags & ~31u) && (((flags & 6u) == 2u && count != 0) || ((flags & 6u) == 4u && count == 0));
    }

    // Validate before reporting busy for a released source, so a malformed or
    // foreign-session marker can never become an accepted retirement ack.
    inline int native_marker_queue_policy(const session_native_frame& p, uint64_t session,
        uint64_t operation_fence, uint64_t restore_after_frame, bool source_released) {
        if (p.size != 64 || p.version != 1 || !p.source_frame || !p.session || p.session != session ||
            !p.width || !p.height || p.width > 16384 || p.height > 16384 || p.reserved || (p.flags & ~1u) ||
            (p.restore_serial && (p.restore_serial != operation_fence ||
                (!(p.flags & 1u) && p.source_frame <= restore_after_frame)))) return -201;
        return source_released ? 1 : 0;
    }

}
