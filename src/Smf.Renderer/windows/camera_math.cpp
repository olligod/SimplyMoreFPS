#include "session_internal.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace session {

    DXGI_FORMAT compatible_format(DXGI_FORMAT f) {
        if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    bool projection(const session_pose& p, uint32_t width, uint32_t height, affine& a) {
        if (p.size != sizeof(session_pose) || p.version != 2 || !p.epoch || !p.camera_id || !p.frame_id) return false;
        if (!p.model_revision || p.reserved || p.map_id < 0) return false;
        if (!std::isfinite(p.root_x) || !std::isfinite(p.root_y) || !std::isfinite(p.root_z)) return false;
        if (!std::isfinite(p.root_size) || p.root_size <= 0) return false;
        if (!std::isfinite(p.orthographic_size) || p.orthographic_size <= 0) return false;
        if (p.pixel_x != 0 || p.pixel_y != 0 || p.pixel_width != width || p.pixel_height != height) return false;

        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(p.world_to_camera[i]) || !std::isfinite(p.projection[i])) return false;
        }

        if (std::abs(p.projection[3]) > 1e-7 || std::abs(p.projection[7]) > 1e-7) return false;
        if (std::abs(p.projection[11]) > 1e-7 || std::abs(p.projection[15] - 1) > 1e-7) return false;

        double combined[16]{};
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                for (int k = 0; k < 4; ++k) {
                    combined[column * 4 + row] += double(p.projection[k * 4 + row]) * p.world_to_camera[column * 4 + k];
                }
            }
        }

        if (std::abs(combined[3]) > 1e-7 || std::abs(combined[11]) > 1e-7 || std::abs(combined[15]) < 1e-10) return false;

        // A float 90-degree view rotation leaves FLT_EPSILON-sized cross terms. Allow that
        // much relative to each ground basis so zoom cannot magnify it into a fake tilt.
        constexpr double angular_roundoff = 4 * std::numeric_limits<float>::epsilon();
        if (std::abs(combined[4]) > angular_roundoff * std::hypot(combined[0], combined[8])) return false;
        if (std::abs(combined[5]) > angular_roundoff * std::hypot(combined[1], combined[9])) return false;

        const double sx = double(width) * 0.5 / combined[15];
        const double sy = -double(height) * 0.5 / combined[15];
        a = {combined[0] * sx, combined[8] * sx, combined[12] * sx + width * 0.5,
             combined[1] * sy, combined[9] * sy, combined[13] * sy + height * 0.5};
        return std::isfinite(a.a) && std::isfinite(a.b) && std::isfinite(a.c) &&
            std::isfinite(a.d) && std::isfinite(a.e) && std::isfinite(a.f) && std::abs(a.a * a.e - a.b * a.d) > 1e-10;
    }

    bool root_projection(const model& m, double x, double z, double projection_half_height, affine& result) {
        if (!m.revision || !std::isfinite(x) || !std::isfinite(z)) return false;
        if (!std::isfinite(projection_half_height) || projection_half_height <= 0) return false;

        if (x == m.x && z == m.z && projection_half_height == m.projection_half_height) {
            result = m.nominal;
            return true;
        }

        const double scale = m.projection_half_height / projection_half_height;
        const double cx = m.width * .5;
        const double cy = m.height * .5;
        const affine& n = m.nominal;

        result = {n.a * scale, n.b * scale,
            cx + scale * (n.c - cx + n.a * (m.x - x) + n.b * (m.z - z)),
            n.d * scale, n.e * scale,
            cy + scale * (n.f - cy + n.d * (m.x - x) + n.e * (m.z - z))};
        return std::isfinite(result.a) && std::isfinite(result.b) && std::isfinite(result.c) &&
            std::isfinite(result.d) && std::isfinite(result.e) && std::isfinite(result.f);
    }

    static bool nearly_equal(double actual, double expected) {
        return std::abs(actual - expected) <= 1e-6 * (1 + std::abs(expected));
    }

    bool source_model(const model& m, const session_pose& p, const affine& actual, affine& nominal) {
        if (p.epoch != m.epoch || p.model_revision != m.revision || p.map_id != m.map_id) return false;
        if (!root_projection(m, p.root_x, p.root_z, p.orthographic_size, nominal)) return false;
        // Only the ground-plane basis has to agree. Translation and shake stay in source pixels.
        return nearly_equal(actual.a, nominal.a) && nearly_equal(actual.b, nominal.b) &&
            nearly_equal(actual.d, nominal.d) && nearly_equal(actual.e, nominal.e);
    }

    bool mapping(const affine& reference, affine source, uint32_t height, bool flip, D2D_MATRIX_3X2_F& m) {
        if (flip) {
            source.d = -source.d;
            source.e = -source.e;
            source.f = height - source.f;
        }

        if (!flip && std::memcmp(&reference, &source, sizeof(affine)) == 0) {
            m = {1, 0, 0, 1, 0, 0};
            return true;
        }

        const double det = source.a * source.e - source.b * source.d;
        if (!std::isfinite(det) || std::abs(det) < 1e-10) return false;
        const affine inverse{source.e / det, -source.b / det, (source.b * source.f - source.e * source.c) / det,
            -source.d / det, source.a / det, (source.d * source.c - source.a * source.f) / det};

        const double values[] = {
            reference.a * inverse.a + reference.b * inverse.d,
            reference.d * inverse.a + reference.e * inverse.d,
            reference.a * inverse.b + reference.b * inverse.e,
            reference.d * inverse.b + reference.e * inverse.e,
            reference.a * inverse.c + reference.b * inverse.f + reference.c,
            reference.d * inverse.c + reference.e * inverse.f + reference.f};

        float* out = &m._11;
        for (int i = 0; i < 6; ++i) {
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7) return false;
            out[i] = static_cast<float>(values[i]);
        }

        return true;
    }

}
