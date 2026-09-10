#pragma once
#include "session_packets.h"
#include "../common/projection_math.h"
#include <cmath>
#include <cstring>

// Pure policies with no Metal, AppKit or Unity dependency; the tests run them without a display.
namespace mac {

    inline bool valid_absent_world(const session_frame& frame) {
        session_pose empty{};
        // Unity allocates both RTs for every generation, including title screens,
        // so absence is judged from the dispatch flags and a zero pose.
        return !(frame.flags & 1) && (frame.flags & 4) && !(frame.flags & 2) &&
            frame.world_dispatches == 0 && !std::memcmp(&frame.pose, &empty, sizeof(empty));
    }

    struct affine {
        double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    };

    inline bool valid_affine(const affine& m) {
        const double values[] = {m.a, m.b, m.c, m.d, m.e, m.f};
        for (double v : values) {
            if (!std::isfinite(v) || std::abs(v) > 1e12) return false;
        }
        return std::abs(m.a * m.e - m.b * m.d) > 1e-10;
    }

    inline bool invert_affine(const affine& m, affine& out) {
        if (!valid_affine(m)) return false;

        double det = m.a * m.e - m.b * m.d;
        out = smf_projection::inverse(m, det);
        return valid_affine(out);
    }

    inline affine multiply_affine(const affine& l, const affine& r) {
        return smf_projection::multiply(l, r);
    }

    inline bool valid_cache(const session_frame& frame, const session_cache& previous) {
        const auto& c = frame.cache;
        if (!(frame.flags & 1)) {
            session_cache empty{};
            return !std::memcmp(&c, &empty, sizeof(c));
        }

        if (!(c.flags & 1) || (c.flags & ~3u) || c.reserved || !c.texture || !c.serial || c.serial > frame.source_frame ||
            c.texture == frame.world_texture || c.texture == frame.hud_texture || !c.width || !c.height ||
            c.width > 16384 || c.height > 16384 || uint64_t(c.width) * c.height * 4 > 64ull * 1024 * 1024) return false;

        for (double v : c.affine) {
            if (!std::isfinite(v) || std::abs(v) > 1e7) return false;
        }
        if (!valid_affine({c.affine[0], c.affine[1], c.affine[2], c.affine[3], c.affine[4], c.affine[5]})) return false;
        if (previous.serial && (c.serial < previous.serial || c.width != previous.width ||
            c.height != previous.height || c.flags != previous.flags)) return false;
        if (c.serial == previous.serial && std::memcmp(&c, &previous, sizeof(c))) return false;

        return true;
    }

    // World X/Z to pixel affine for a validated top-down orthographic pose.
    inline bool pose_projection(const session_pose& p, uint32_t width, uint32_t height, affine& out) {
        if (p.size != 264 || p.version != 2 || !p.frame_id || !p.epoch || !p.camera_id || !p.model_revision ||
            p.map_id < 0 || p.reserved || p.root_size <= 0 || !std::isfinite(p.root_size) ||
            !std::isfinite(p.root_x) || !std::isfinite(p.root_y) || !std::isfinite(p.root_z) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.pixel_x || p.pixel_y ||
            p.pixel_width != width || p.pixel_height != height ||
            !std::isfinite(p.orthographic_size) || p.orthographic_size <= 0) return false;

        if (!smf_projection::ground_projection(p.projection, p.world_to_camera, width, height, out)) return false;
        return valid_affine(out);
    }

    // The projection accepted for a generation, re-based to any camera root position and zoom.
    struct camera_model {
        affine nominal{};
        uint64_t revision = 0, epoch = 0;
        int map = -1;
        double x = 0, z = 0, logical_root_size = 0, projection_half_height = 0;
        uint32_t width = 0, height = 0;

        bool root(double root_x, double root_z, double half_height, affine& out) const {
            if (!revision || !std::isfinite(root_x) || !std::isfinite(root_z) || !std::isfinite(half_height) || half_height <= 0) return false;

            if (root_x == x && root_z == z && half_height == projection_half_height) {
                out = nominal;
                return true;
            }

            out = smf_projection::root_projection(nominal, projection_half_height / half_height,
                width, height, x - root_x, z - root_z);
            return valid_affine(out);
        }

        bool accept(const session_pose& p, const affine& measured, uint32_t w, uint32_t h) {
            if (!revision) {
                nominal = measured;
                nominal.c += measured.a * (p.x - p.root_x) + measured.b * (p.z - p.root_z);
                nominal.f += measured.d * (p.x - p.root_x) + measured.e * (p.z - p.root_z);

                revision = p.model_revision;
                map = p.map_id;
                x = p.root_x;
                z = p.root_z;
                logical_root_size = p.root_size;
                projection_half_height = p.orthographic_size;
                epoch = p.epoch;
                width = w;
                height = h;
                return true;
            }

            affine expected;
            if (p.epoch != epoch || p.model_revision != revision || p.map_id != map || width != w || height != h ||
                !root(p.root_x, p.root_z, p.orthographic_size, expected)) return false;

            auto near = [](double v, double e) { return std::abs(v - e) <= 1e-6 * (1 + std::abs(e)); };
            return near(measured.a, expected.a) && near(measured.b, expected.b) &&
                near(measured.d, expected.d) && near(measured.e, expected.e);
        }
    };

}
