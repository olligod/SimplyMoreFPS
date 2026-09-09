#pragma once
#include <cstdint>

namespace linux_session {

    enum class draw_outcome { drawn, stale_camera, stale_geometry, failed };

    inline bool failed_activation_submission(draw_outcome outcome, bool submitted, bool cancelled) {
        return !submitted && !cancelled && outcome != draw_outcome::stale_geometry;
    }

    inline bool matching_camera_tuple(uint64_t image_epoch, int32_t image_map, uint64_t desired_epoch, int32_t desired_map) {
        return image_epoch == desired_epoch && image_map == desired_map;
    }

    inline bool stale_camera_tuple(bool camera, uint64_t image_epoch, int32_t image_map, uint64_t desired_epoch, int32_t desired_map) {
        return camera && !matching_camera_tuple(image_epoch, image_map, desired_epoch, desired_map);
    }

}
