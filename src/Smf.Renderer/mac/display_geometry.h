#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mac {

    struct image_viewport {
        double x = 0, y = 0, width = 0, height = 0, scale_x = 0, scale_y = 0;
    };

    // Unity's fullscreen aspect fit. Coordinates are view points; the scales map
    // points inside the displayed image back to Unity image pixels.
    inline bool fit_image(double x, double y, double width, double height, uint32_t image_width, uint32_t image_height, image_viewport& out) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0 || !image_width || !image_height || image_width > 16384 || image_height > 16384) return false;

        double ratio = std::min(width / image_width, height / image_height);
        double w = image_width * ratio;
        double h = image_height * ratio;
        out = {x + (width - w) * .5, y + (height - h) * .5, w, h, image_width / w, image_height / h};
        return std::isfinite(out.scale_x) && std::isfinite(out.scale_y) && out.scale_x > 0 && out.scale_y > 0;
    }

}
