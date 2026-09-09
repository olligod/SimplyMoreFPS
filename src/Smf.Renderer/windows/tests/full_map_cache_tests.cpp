// Real production mapping and root_projection; numerical only, no D3D calls.
#include "../session_internal.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace session;

struct point {
    double x;
    double y;
};

static point apply(const D2D_MATRIX_3X2_F& m, point p) {
    return {m._11 * p.x + m._21 * p.y + m._31, m._12 * p.x + m._22 * p.y + m._32};
}

static void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static model model_for(const session_pose& p, const affine& nominal, uint32_t width, uint32_t height) {
    model m;
    m.nominal = nominal;
    m.x = p.root_x;
    m.z = p.root_z;
    m.projection_half_height = p.orthographic_size;
    m.epoch = p.epoch;
    m.revision = p.model_revision;
    m.map_id = p.map_id;
    m.width = width;
    m.height = height;

    return m;
}

// A pose is accepted when its ground basis agrees with the model's nominal projection.
static bool accepts(const model& m, const session_pose& pose, const affine& actual) {
    affine nominal;
    return source_model(m, pose, actual, nominal);
}

int main() {
    // A pose captured live at root/ortho 0.5. The float 90-degree view rotation leaves
    // -FLT_EPSILON in camera Y; multiplying by projection[5] = 2 must not read as tilt.
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
        static_assert(sizeof(raw) == sizeof(p), "captured pose 264");
        std::memcpy(&p, raw, sizeof(p));

        affine actual;
        check(projection(p, 1280, 720, actual), "captured pose projects");
        check(std::abs(actual.a - 720) < 1e-9 && std::abs(actual.e + 720) < 1e-9, "captured pose basis");

        const model m = model_for(p, actual, 1280, 720);
        check(accepts(m, p, actual), "captured pose accepted");

        auto invalid = p;
        invalid.world_to_camera[5] = .001f;
        check(!projection(invalid, 1280, 720, actual), "tilted view rejected");
        invalid = p;
        invalid.projection[3] = .01f;
        check(!projection(invalid, 1280, 720, actual), "perspective projection rejected");

        invalid = p;
        invalid.world_to_camera[8] = .01f;
        check(projection(invalid, 1280, 720, actual), "sheared view still projects");
        check(!accepts(m, invalid, actual), "sheared view rejected by the model");

        for (float half_height : {.5f, 1.f, 24.f, 100.f}) {
            auto zoom = p;
            zoom.orthographic_size = half_height;
            zoom.root_size = half_height;
            zoom.projection[0] = float(720. / (1280. * half_height));
            zoom.projection[5] = 1.f / half_height;
            check(projection(zoom, 1280, 720, actual), "zoomed pose projects");
            check(accepts(m, zoom, actual), "zoomed pose accepted");
        }
    }

    // Camera+ observed logical 24 / ortho 35.959 -> logical 20.197 / ortho 26.025.
    // Different logical/projection ratios are valid; changed rotation, shear, map,
    // epoch and an invalid size are not.
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
        p.x = 53.5f;
        p.z = 48.5f;
        p.root_x = 53.5;
        p.root_z = 48.5;
        p.root_size = 24;
        p.orthographic_size = 35.9591827f;
        p.pixel_width = float(w);
        p.pixel_height = float(h);
        p.world_to_camera[0] = 1;
        p.world_to_camera[9] = 1;
        p.world_to_camera[6] = -1;
        p.world_to_camera[15] = 1;
        p.world_to_camera[12] = -p.x;
        p.world_to_camera[13] = -p.z;

        auto project = [&]() {
            p.projection[0] = float(double(h) / (p.orthographic_size * w));
            p.projection[5] = 1.f / p.orthographic_size;
            p.projection[10] = -.01f;
            p.projection[15] = 1;
        };

        project();
        affine a;
        check(projection(p, w, h, a), "synthetic pose projects");
        const model m = model_for(p, a, w, h);
        check(accepts(m, p, a), "synthetic pose accepted");

        affine identity;
        check(root_projection(m, p.root_x, p.root_z, p.orthographic_size, identity), "root projection at the model pose");
        check(std::memcmp(&identity, &m.nominal, sizeof(identity)) == 0, "model pose maps to the nominal affine");

        p.root_size = 20.1970444;
        p.orthographic_size = 26.024931f;
        project();
        check(projection(p, w, h, a), "zoomed pose projects");
        check(accepts(m, p, a), "different logical/projection ratio accepted");

        affine desired;
        check(root_projection(m, p.root_x, p.root_z, p.orthographic_size, desired), "zoomed root projection");
        check(std::abs(desired.a - double(h) / (2 * p.orthographic_size)) < 1e-5, "zoomed scale");

        auto bad = a;
        bad.b += .1;
        check(!accepts(m, p, bad), "rotation rejected");
        bad = a;
        bad.d += .1;
        check(!accepts(m, p, bad), "shear rejected");

        bad = a;
        bad.a *= .99;
        bad.e *= 1.01;
        check(!accepts(m, p, bad), "anisotropic scale rejected");

        ++p.map_id;
        check(!accepts(m, p, a), "map change rejected");
        --p.map_id;
        ++p.epoch;
        check(!accepts(m, p, a), "epoch change rejected");

        --p.epoch;
        p.orthographic_size = 0;
        check(!projection(p, w, h, a), "zero size does not project");
        check(!root_projection(m, p.root_x, p.root_z, 0, desired), "zero size has no root projection");
    }

    // Worker D*inv(N0), source N0*inv(N) and cache N*inv(C) must compose to D*inv(C).
    model m{};
    m.revision = 1;
    m.valid = true;
    m.width = 1280;
    m.height = 720;
    m.x = 100;
    m.z = 150;
    m.projection_half_height = 25;
    m.nominal = {2, .3, -60, .2, -2, 620};

    const affine caches[] = {{8, 0, 0, 0, -8, 2048}, {6, .2, 180, -.1, -6, 1970}};
    const double positions[][3] = {{100, 150, 25}, {10, 250, 12}, {270, 90, 40}, {-50, 500, 70}};
    const point points[] = {{0, 0}, {2048, 0}, {2048, 2048}, {0, 2048}, {170, 830}};
    unsigned comparisons = 0;

    for (const auto& cache : caches) {
        for (bool flip : {false, true}) {
            for (const auto& source_pose : positions) {
                for (const auto& desired_pose : positions) {
                    affine nominal{};
                    affine desired{};
                    check(root_projection(m, source_pose[0], source_pose[1], source_pose[2], nominal), "source nominal");
                    check(root_projection(m, desired_pose[0], desired_pose[1], desired_pose[2], desired), "worker desired");

                    D2D_MATRIX_3X2_F child{};
                    D2D_MATRIX_3X2_F source{};
                    D2D_MATRIX_3X2_F worker{};
                    D2D_MATRIX_3X2_F direct{};

                    check(mapping(nominal, cache, 2048, flip, child), "child N inverse C");
                    check(mapping(m.nominal, nominal, 720, false, source), "source N0 inverse N");
                    check(mapping(desired, m.nominal, 720, false, worker), "worker D inverse N0");
                    check(mapping(desired, cache, 2048, flip, direct), "direct D inverse C");

                    for (point p : points) {
                        const point composed = apply(worker, apply(source, apply(child, p)));
                        const point expected = apply(direct, p);
                        check(std::abs(composed.x - expected.x) < .002 && std::abs(composed.y - expected.y) < .002,
                            "cache stays world-anchored across source pan/zoom and independent worker motion");
                        ++comparisons;
                    }
                }
            }
        }
    }

    std::printf("PASS %u cache transform composition checks, pan/zoom/refresh/row-flip; no graphics\n", comparisons);
    return 0;
}
