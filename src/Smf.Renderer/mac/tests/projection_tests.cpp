#include "../core.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace mac;

int main() {
    // A pose captured live at root size 0.5. The float 90-degree view rotation
    // leaves -FLT_EPSILON in camera Y; multiplying it by projection[5] = 2 must
    // not turn it into an unsupported tilt.
    {
        const unsigned char raw[] = {
            0x08, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x1f, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xae, 0x12, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x88, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x42,
            0x00, 0x00, 0x70, 0x41, 0x00, 0x00, 0xa0, 0x42, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xa0, 0x44, 0x00, 0x00, 0x34, 0x44, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x80, 0x3f, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0xc2, 0x00, 0x00, 0xa0, 0xc2, 0x0a, 0x00, 0x70, 0xc1,
            0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x90, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xc1, 0x0f, 0xfc, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x20, 0xf8, 0x81, 0xbf, 0x00, 0x00,
            0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x40, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x2e, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x3f};

        session_pose p{};
        static_assert(sizeof(raw) == sizeof(p), "captured Pose264");
        std::memcpy(&p, raw, sizeof(p));

        affine actual;
        assert(pose_projection(p, 1280, 720, actual));
        assert(std::abs(actual.a - 720) < 1e-9 && std::abs(actual.e + 720) < 1e-9);

        camera_model model;
        assert(model.accept(p, actual, 1280, 720));

        auto invalid = p;
        invalid.world_to_camera[5] = .001f;
        assert(!pose_projection(invalid, 1280, 720, actual));
        invalid = p;
        invalid.projection[3] = .01f;
        assert(!pose_projection(invalid, 1280, 720, actual));

        invalid = p;
        invalid.world_to_camera[8] = .01f;
        assert(pose_projection(invalid, 1280, 720, actual));
        assert(!model.accept(invalid, actual, 1280, 720));

        for (float half_height : {.5f, 1.f, 24.f, 100.f}) {
            auto zoom = p;
            zoom.root_size = zoom.orthographic_size = half_height;
            zoom.projection[0] = float(720. / (1280. * half_height));
            zoom.projection[5] = 1.f / half_height;
            assert(pose_projection(zoom, 1280, 720, actual));
            assert(model.accept(zoom, actual, 1280, 720));
        }
    }

    // Camera+ observed logical 24 / ortho 35.959 -> logical 20.197 / ortho 26.025.
    // Different logical/projection ratios are valid; a changed rotation, shear,
    // map or epoch and an invalid size are not.
    {
        const uint32_t w = 1280;
        const uint32_t h = 720;
        session_pose p{};
        p.size = 264;
        p.version = 2;
        p.frame_id = 1;
        p.epoch = 7;
        p.camera_id = 1;
        p.model_revision = 9;
        p.map_id = 0;
        p.root_x = p.x = 53.5;
        p.root_z = p.z = 48.5;
        p.root_size = 24;
        p.orthographic_size = 35.9591827f;
        p.pixel_width = w;
        p.pixel_height = h;
        p.world_to_camera[0] = 1;
        p.world_to_camera[9] = 1;
        p.world_to_camera[6] = -1;
        p.world_to_camera[15] = 1;
        p.world_to_camera[12] = -p.x;
        p.world_to_camera[13] = -p.z;

        auto project = [&] {
            p.projection[0] = float(double(h) / (p.orthographic_size * w));
            p.projection[5] = 1.f / p.orthographic_size;
            p.projection[10] = -.01f;
            p.projection[15] = 1;
        };

        project();
        affine a;
        assert(pose_projection(p, w, h, a));

        camera_model model;
        assert(model.accept(p, a, w, h));
        affine identity;
        assert(model.root(p.root_x, p.root_z, p.orthographic_size, identity));
        assert(std::memcmp(&identity, &model.nominal, sizeof(identity)) == 0);

        p.root_size = 20.1970444;
        p.orthographic_size = 26.024931f;
        project();
        assert(pose_projection(p, w, h, a));
        assert(model.accept(p, a, w, h));

        affine desired;
        assert(model.root(p.root_x, p.root_z, p.orthographic_size, desired));
        assert(std::abs(desired.a - double(h) / (2 * p.orthographic_size)) < 1e-5);

        auto bad = a;
        bad.b += .1;
        assert(!model.accept(p, bad, w, h));
        bad = a;
        bad.d += .1;
        assert(!model.accept(p, bad, w, h));

        bad = a;
        bad.a *= .99;
        bad.e *= 1.01;
        assert(!model.accept(p, bad, w, h));

        ++p.map_id;
        assert(!model.accept(p, a, w, h));
        --p.map_id;
        ++p.epoch;
        assert(!model.accept(p, a, w, h));
        --p.epoch;

        p.orthographic_size = 0;
        assert(!pose_projection(p, w, h, a));
        assert(!model.root(p.root_x, p.root_z, 0, desired));
    }

    session_pose pose{};
    affine measured;
    auto eval = [](const affine& a, double x, double y) {
        return std::array<double, 2>{a.a * x + a.b * y + a.c, a.d * x + a.e * y + a.f};
    };

    // Real pixel evidence: an unsigned -height once produced +89478472.99999955
    // instead of -15. Exercise the validated pose to projection path, not a
    // hand-written affine, at the native and two resized aspect ratios.
    for (const auto dimensions : {std::array<uint32_t, 2>{1280, 720}, {1024, 768}, {720, 1280}}) {
        uint32_t w = dimensions[0];
        uint32_t h = dimensions[1];
        pose = {};
        pose.size = 264;
        pose.version = 2;
        pose.frame_id = 5077;
        pose.epoch = 1;
        pose.camera_id = 1;
        pose.model_revision = 1;
        pose.map_id = 0;
        pose.x = 53.5f;
        pose.z = 48.5f;
        pose.root_x = 53.5;
        pose.root_z = 48.5;
        pose.root_size = 24;
        pose.orthographic_size = 24;
        pose.pixel_width = float(w);
        pose.pixel_height = float(h);

        pose.world_to_camera[0] = 1;
        pose.world_to_camera[9] = 1;
        pose.world_to_camera[6] = -1;
        pose.world_to_camera[15] = 1;
        pose.world_to_camera[12] = -pose.x;
        pose.world_to_camera[13] = -pose.z;

        pose.projection[0] = float(double(h) / (24 * w));
        pose.projection[5] = 1.f / 24;
        pose.projection[10] = -.01f;
        pose.projection[15] = 1;

        assert(pose_projection(pose, w, h, measured));
        double scale = double(h) / 48;
        assert(measured.e < 0 && std::abs(measured.e + scale) < 1e-5 && std::abs(measured.a - scale) < 1e-5);

        auto center = eval(measured, pose.x, pose.z);
        assert(std::abs(center[0] - w * .5) < 1e-7 && std::abs(center[1] - h * .5) < 1e-7);

        auto north = eval(measured, pose.x, pose.z + 10);
        auto south = eval(measured, pose.x, pose.z - 10);
        assert(std::abs((south[1] - north[1]) - 20 * scale) < 1e-4);

        camera_model measured_model;
        assert(measured_model.accept(pose, measured, w, h));

        affine full_map{2048. / 108, 0, 2048. * 4 / 108, 0, -2048. / 108, 2048. * 104 / 108};
        for (const auto move : {std::array<double, 3>{70.3, 48.5, 24}, {70.3, 2, 30}, {70.3, 98, 18}}) {
            affine wanted;
            affine back;
            assert(measured_model.root(move[0], move[1], move[2], wanted));
            assert(invert_affine(wanted, back));
            affine cached = multiply_affine(full_map, back);

            for (double z : {10., 40., 55., 90.}) {
                auto displayed = eval(wanted, 99, z);
                auto direct = eval(full_map, 99, z);
                auto sampled = eval(cached, displayed[0], displayed[1]);
                assert(std::abs(direct[0] - sampled[0]) < 1e-8 && std::abs(direct[1] - sampled[1]) < 1e-8);
            }
        }

        if (w == 1280 && h == 720) {
            double old_scale = double(uint32_t(0) - h) * .5 * pose.projection[5];
            assert(std::abs(old_scale - 89478472.99999955) < 1e-5); // the exact bad coefficient once observed
        }
    }

    std::cout << "PASS real Pose264 projection at three aspect ratios and pan/zoom cache composition\n";
}
