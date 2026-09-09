#include "metal_owner.h"
#include "present_observer.h"

namespace mac {

    bool original_observer_available() {
        return present_observer::available() || drawable_observer::available();
    }

    bool poll_original_frame(original_frame& value) {
        value = {};
        present_observer::presented_frame observed;
        if (present_observer::poll(observed)) {
            value = {observed.session, observed.frame, observed.generation, observed.content, observed.restore,
                observed.serial, observed.drawable, observed.acquired_ns, observed.presented_ns};
            return true;
        }

        drawable_observer::presented_frame presented;
        if (!drawable_observer::poll(presented) || !presented.session || !presented.drawable || !presented.serial ||
            !presented.acquired_ns || !presented.presented_ns || !std::isfinite(presented.presented_time) || presented.presented_time <= 0) return false;

        value = {presented.session, presented.frame, presented.generation, presented.content, presented.restore,
            presented.serial, presented.drawable, presented.acquired_ns, presented.presented_ns};
        return true;
    }

    void arm_original_frame(const session_native_frame& value) {
        // A begin-only marker expires old leases; it never claims a complete native frame.
        if (value.flags & 1) return;
        drawable_observer::arm({value.session, value.source_frame, value.generation, value.content_revision,
            value.restore_serial, value.width, value.height});
    }

}
