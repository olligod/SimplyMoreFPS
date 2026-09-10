#include "session_internal.h"
#include "../common/projection_math.h"
#include <cmath>
#include <cstring>

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

        if (!smf_projection::ground_projection(p.projection, p.world_to_camera, width, height, a)) return false;
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

        result = smf_projection::root_projection(m.nominal, m.projection_half_height / projection_half_height,
            m.width, m.height, m.x - x, m.z - z);
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
        const affine inverse = smf_projection::inverse(source, det);
        const affine mapped = smf_projection::multiply(reference, inverse);
        const double values[] = {mapped.a, mapped.d, mapped.b, mapped.e, mapped.c, mapped.f};

        float* out = &m._11;
        for (int i = 0; i < 6; ++i) {
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7) return false;
            out[i] = static_cast<float>(values[i]);
        }

        return true;
    }

}
