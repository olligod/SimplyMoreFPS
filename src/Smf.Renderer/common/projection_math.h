#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace smf_projection {

    // Platform wrappers retain packet admission and output coefficient limits.
    template <class Affine>
    bool ground_projection(const float* projection, const float* world_to_camera,
        uint32_t width, uint32_t height, Affine& out) {
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(world_to_camera[i]) || !std::isfinite(projection[i])) return false;
        }

        if (std::abs(projection[3]) > 1e-7 || std::abs(projection[7]) > 1e-7 ||
            std::abs(projection[11]) > 1e-7 || std::abs(projection[15] - 1) > 1e-7) return false;

        double combined[16]{};
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                for (int k = 0; k < 4; ++k) {
                    combined[column * 4 + row] += double(projection[k * 4 + row]) * world_to_camera[column * 4 + k];
                }
            }
        }

        if (std::abs(combined[3]) > 1e-7 || std::abs(combined[11]) > 1e-7 || std::abs(combined[15]) < 1e-10) return false;

        // Float right-angle rotations leave residuals; compare angles so zoom cannot amplify them.
        constexpr double angular_roundoff = 4 * std::numeric_limits<float>::epsilon();
        if (std::abs(combined[4]) > angular_roundoff * std::hypot(combined[0], combined[8]) ||
            std::abs(combined[5]) > angular_roundoff * std::hypot(combined[1], combined[9])) return false;

        const double sx = double(width) * .5 / combined[15];
        // Convert before negating the unsigned height.
        const double sy = -double(height) * .5 / combined[15];
        out = {combined[0] * sx, combined[8] * sx, combined[12] * sx + width * .5,
            combined[1] * sy, combined[9] * sy, combined[13] * sy + height * .5};
        return true;
    }

    template <class Affine>
    Affine root_projection(const Affine& nominal, double scale, uint32_t width, uint32_t height,
        double delta_x, double delta_z) {
        const double cx = width * .5;
        const double cy = height * .5;
        return {nominal.a * scale, nominal.b * scale,
            cx + scale * (nominal.c - cx + nominal.a * delta_x + nominal.b * delta_z),
            nominal.d * scale, nominal.e * scale,
            cy + scale * (nominal.f - cy + nominal.d * delta_x + nominal.e * delta_z)};
    }

    // Callers validate determinants and results using their platform's admission limits.
    template <class Affine>
    Affine inverse(const Affine& a, double determinant) {
        return {a.e / determinant, -a.b / determinant, (a.b * a.f - a.e * a.c) / determinant,
            -a.d / determinant, a.a / determinant, (a.d * a.c - a.a * a.f) / determinant};
    }

    template <class Affine>
    Affine multiply(const Affine& a, const Affine& b) {
        return {a.a * b.a + a.b * b.d, a.a * b.b + a.b * b.e, a.a * b.c + a.b * b.f + a.c,
            a.d * b.a + a.e * b.d, a.d * b.b + a.e * b.e, a.d * b.c + a.e * b.f + a.f};
    }

}
