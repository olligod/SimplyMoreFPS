#pragma once
#include "camera_packets.h"
#include <cmath>

namespace camera_control {
    namespace input_policy {

        inline bool valid(const smf_control_policy& p) {
            if (p.size != sizeof(p) || p.version != 1 || !p.epoch || !p.revision || p.rect_count > 64 ||
                (p.flags & ~127u) || !std::isfinite(p.ui_scale) || p.ui_scale <= 0 ||
                !std::isfinite(p.inspect_height) || p.inspect_height < 0) return false;

            for (uint32_t i = 0; i < p.rect_count; ++i) {
                const auto& r = p.rects[i];
                if (!std::isfinite(r.left) || !std::isfinite(r.top) || !std::isfinite(r.right) || !std::isfinite(r.bottom) ||
                    r.right < r.left || r.bottom < r.top) return false;
            }
            return true;
        }

        inline bool valid_key_impulse(const smf_control_impulse& p, uint64_t last_sequence) {
            // Wheel input comes from the native observer only.
            return p.size == sizeof(p) && p.version == 1 && p.epoch && p.sequence > last_sequence &&
                !p.reserved && !(p.flags & ~3u) && p.flags && p.wheel_delta == 0;
        }

        inline bool pointer_over_ui(const smf_control_policy& p, double x, double y) {
            for (uint32_t i = 0; i < p.rect_count; ++i) {
                const auto& r = p.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
            }
            return false;
        }

        inline void apply(const smf_control_policy& p, uint64_t epoch, double x, double y, smf_camera_input& input) {
            if (p.epoch != epoch) return;
            if (p.flags & 1) input.flags |= SMF_CAMERA_ALLOW_EDGE_SCROLL;
            if (p.flags & 2) input.flags |= SMF_CAMERA_FULLSCREEN;
            input.inspect_pane_height = p.inspect_height;
            if (pointer_over_ui(p, x, y)) input.flags |= SMF_CAMERA_POINTER_OVER_UI;
        }

        inline bool wheel_position_allowed(const smf_control_policy& p, uint64_t epoch, double x, double y) {
            if (p.epoch != epoch || (p.flags & 12u) != 8u || x < 0 || y < 0 || x >= p.width || y >= p.height) return false;
            return !pointer_over_ui(p, x, y);
        }

    }
}
