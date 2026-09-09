#include "../linux_core.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace linux_session;

struct test_packet {
    uint64_t generation = 0;
    uint64_t frame = 0;
};

int main() {
    // Live Pose264 captured at root, ortho 0.5, 2026-09-09. The float 90 degree view
    // rotation leaves -FLT_EPSILON in camera Y; times projection[5] = 2 it must not read as tilt.
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
        pose_packet p{};
        static_assert(sizeof(raw) == sizeof(p), "captured Pose264");
        std::memcpy(&p, raw, sizeof(p));

        affine measured;
        assert(projection(p, 1280, 720, measured));
        assert(std::abs(measured.a - 720) < 1e-9);
        assert(std::abs(measured.e + 720) < 1e-9);

        camera_model model;
        assert(model.accept(p, measured, 1280, 720));

        auto invalid = p;
        invalid.world_to_camera[5] = .001f;
        assert(!projection(invalid, 1280, 720, measured));
        invalid = p;
        invalid.projection[3] = .01f;
        assert(!projection(invalid, 1280, 720, measured));
        invalid = p;
        invalid.world_to_camera[8] = .01f;
        assert(projection(invalid, 1280, 720, measured));
        assert(!model.accept(invalid, measured, 1280, 720));

        for (float half_height : {.5f, 1.f, 24.f, 100.f}) {
            auto zoom = p;
            zoom.root_size = half_height;
            zoom.orthographic_size = half_height;
            zoom.projection[0] = float(720. / (1280. * half_height));
            zoom.projection[5] = 1.f / half_height;

            assert(projection(zoom, 1280, 720, measured));
            assert(model.accept(zoom, measured, 1280, 720));
        }
    }

    // Camera+ observed logical 24 / ortho 35.959 going to logical 20.197 / ortho 26.025.
    // Different logical/projection ratios are valid; changed rotation, shear, map,
    // resource epoch and an invalid size are not.
    {
        const uint32_t w = 1280;
        const uint32_t h = 720;
        pose_packet p{};
        p.size = 264;
        p.version = 2;
        p.frame_id = 1;
        p.epoch = 7;
        p.camera_id = 1;
        p.model_revision = 9;
        p.map_id = 0;
        p.x = 53.5;
        p.z = 48.5;
        p.root_x = 53.5;
        p.root_z = 48.5;
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

        auto project = [&]() {
            p.projection[0] = float(double(h) / (p.orthographic_size * w));
            p.projection[5] = 1.f / p.orthographic_size;
            p.projection[10] = -.01f;
            p.projection[15] = 1;
        };

        project();
        affine a;
        assert(projection(p, w, h, a));
        camera_model model;
        assert(model.accept(p, a, w, h));

        affine identity;
        assert(model.root(p.root_x, p.root_z, p.orthographic_size, identity));
        assert(std::memcmp(&identity, &model.nominal, sizeof(identity)) == 0);

        p.root_size = 20.1970444;
        p.orthographic_size = 26.024931f;
        project();

        assert(projection(p, w, h, a));
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
        assert(!projection(p, w, h, a));
        assert(!model.root(p.root_x, p.root_z, 0, desired));
    }

    frame_packet no_world_gui{};
    no_world_gui.flags = 1 | 4;
    no_world_gui.source_frame = 72;
    no_world_gui.pose.size = 264;
    no_world_gui.pose.version = 2;
    no_world_gui.pose.unity_frame = 72;
    no_world_gui.world_texture = 17;
    no_world_gui.hud_texture = 18;
    const auto retained_pose = no_world_gui.pose;

    assert(valid_world_dispatch(no_world_gui));
    assert(!valid_absent_world(no_world_gui));
    assert(!std::memcmp(&retained_pose, &no_world_gui.pose, sizeof(retained_pose))); // the predicate never turns a map pose into no-map

    no_world_gui.flags = 1 | 2;
    assert(!valid_world_dispatch(no_world_gui));
    no_world_gui.flags = 1 | 2 | 4;
    assert(!valid_world_dispatch(no_world_gui));
    no_world_gui.flags = 1;
    assert(!valid_world_dispatch(no_world_gui));
    no_world_gui.world_dispatches = 1;
    no_world_gui.flags = 1 | 4;
    assert(!valid_world_dispatch(no_world_gui));
    no_world_gui.flags = 1 | 2 | 4;
    assert(!valid_world_dispatch(no_world_gui));
    no_world_gui.flags = 1 | 2;
    assert(valid_world_dispatch(no_world_gui));

    // A late cancel through an old slot pointer must not cancel or consume the new
    // packet living in that same memory, and tokens survive many session ids.
    dispatch_pool<test_packet> pool;
    void* old = nullptr;
    int old_token = 0;
    assert(pool.queue({1, 100}, &old, &old_token) == 0);
    assert(pool.occupied() == 1);
    test_packet value;
    assert(pool.cancel(old, old_token) == 0);
    assert(pool.occupied() == 0);

    void* latest = nullptr;
    int latest_token = 0;
    assert(pool.queue({2, 200}, &latest, &latest_token) == 0);
    assert(latest == old);
    assert(latest_token > old_token);
    assert(pool.cancel(old, old_token) == 0);
    assert(!pool.consume(old_token, value));
    assert(pool.occupied() == 1);
    assert(pool.consume(latest_token, value));
    assert(pool.occupied() == 0);
    assert(value.frame == 200);

    void* pointers[32];
    int tokens[32];
    for (int i = 0; i < 32; i++) assert(pool.queue({3, uint64_t(i)}, &pointers[i], &tokens[i]) == 0);
    void* overflow = nullptr;
    int overflow_token = 0;
    assert(pool.queue({4, 0}, &overflow, &overflow_token) == 1);
    assert(pool.occupied() == 32);
    assert(pool.uses(3)); // retirement conservatively sees any queued packet
    assert(pool.uses(4));
    for (int i = 31; i >= 0; i--) assert(pool.cancel(pointers[i], tokens[i]) == 0);
    assert(pool.empty());

    for (uint64_t session = 1; session < 1000; session++) {
        assert(pool.queue({session, session}, &latest, &latest_token) == 0);
        assert(pool.consume(latest_token, value));
        assert(value.generation == session);
        assert(!pool.consume(old_token, value));
    }

    // Neither an accepted stop nor a missing fence can stand in for GPU retirement.
    lease_policy lease;
    assert(lease.begin());
    assert(!lease.reclaim(true));
    assert(lease.publish(true));
    assert(!lease.acquire(false));
    assert(lease.state == lease_published);
    assert(lease.acquire(true));
    assert(lease.hand_back(true));
    assert(!lease.reclaim(false));
    assert(lease.state == lease_returning);
    assert(lease.reclaim(true));
    assert(lease.state == lease_free);

    lease_policy missing;
    assert(missing.begin());
    assert(!missing.publish(false));
    assert(missing.state == lease_quarantined);
    assert(!missing.reclaim(true));
    assert(!missing.retire_unacquired(true));

    lease_policy never_sampled;
    assert(never_sampled.begin());
    assert(never_sampled.publish(true));
    assert(!never_sampled.retire_unacquired(false));
    assert(never_sampled.state == lease_published);
    assert(never_sampled.retire_unacquired(true));
    assert(never_sampled.state == lease_free);

    lease_policy sampled;
    assert(sampled.begin());
    assert(sampled.publish(true));
    assert(sampled.acquire(true));
    assert(!sampled.retire_unacquired(true));
    assert(sampled.state == lease_worker_owned);

    frame_packet title{};
    title.flags = 4;
    title.world_texture = 27;
    title.hud_texture = 28;
    assert(valid_absent_world(title));
    title.world_dispatches = 1;
    assert(!valid_absent_world(title));
    title.world_dispatches = 0;
    title.flags |= 2;
    assert(!valid_absent_world(title));
    title.flags = 4;
    title.pose.map_id = 1;
    assert(!valid_absent_world(title));

    // Source world mapping and cache mapping must agree on the same world point
    // through a simultaneous pan and zoom. The fixed HUD keeps identity on purpose.
    camera_model model;
    model.revision = 1;
    model.width = 1280;
    model.height = 720;
    model.x = 50;
    model.z = 40;
    model.projection_half_height = 20;
    model.nominal = {18, 0, -260, 0, -18, 1080};

    affine desired;
    assert(model.root(57, 35, 10, desired));
    affine inverted;
    assert(inverse(desired, inverted));
    affine cache{4, 0, 0, 0, -4, 400};
    affine cache_from_screen = multiply(cache, inverted);

    auto eval = [](const affine& a, double x, double y) {
        return std::array<double, 2>{a.a * x + a.b * y + a.c, a.d * x + a.e * y + a.f};
    };

    auto screen = eval(desired, 53, 41);
    auto cache_direct = eval(cache, 53, 41);
    auto cache_mapped = eval(cache_from_screen, screen[0], screen[1]);
    assert(std::abs(cache_direct[0] - cache_mapped[0]) < 1e-9);
    assert(std::abs(cache_direct[1] - cache_mapped[1]) < 1e-9);

    assert(!inverse({1, 2, 0, 2, 4, 0}, inverted));
    affine bad{};
    bad.c = std::numeric_limits<double>::infinity();
    assert(!valid(bad));

    pose_packet pose{};
    affine measured;
    assert(!projection(pose, 1280, 720, measured));

    // Observed c19 pixel bug: unsigned -height produced +89478472.99999955 instead of -15.
    // Drive the real pose to projection path at native and resized aspect ratios.
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

        assert(projection(pose, w, h, measured));
        double scale = double(h) / 48;
        assert(measured.e < 0);
        assert(std::abs(measured.e + scale) < 1e-5);
        assert(std::abs(measured.a - scale) < 1e-5);

        auto center = eval(measured, pose.x, pose.z);
        assert(std::abs(center[0] - w * .5) < 1e-7);
        assert(std::abs(center[1] - h * .5) < 1e-7);
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
            assert(inverse(wanted, back));
            affine cached = multiply(full_map, back);

            for (double z : {10., 40., 55., 90.}) {
                auto displayed = eval(wanted, 99, z);
                auto direct = eval(full_map, 99, z);
                auto sampled = eval(cached, displayed[0], displayed[1]);
                assert(std::abs(direct[0] - sampled[0]) < 1e-8);
                assert(std::abs(direct[1] - sampled[1]) < 1e-8);
            }
        }

        if (w == 1280 && h == 720) {
            double old_scale = double(uint32_t(0) - h) * .5 * pose.projection[5];
            assert(std::abs(old_scale - 89478472.99999955) < 1e-5); // the exact bad c19 coefficient
        }
    }

    frame_packet frame{};
    cache_packet previous{};
    assert(valid_cache(frame, previous));

    frame.flags = 1;
    frame.source_frame = 42;
    frame.world_texture = 2;
    frame.hud_texture = 3;
    frame.cache = {4, 40, 512, 512, 1, 0, {4, 0, 0, 0, -4, 512}};
    assert(valid_cache(frame, previous));

    previous = frame.cache;
    frame.cache.serial = 39;
    assert(!valid_cache(frame, previous));
    frame.cache = previous;
    frame.cache.affine[2] = 1;
    assert(!valid_cache(frame, previous));
    frame.cache = previous;
    frame.cache.texture = frame.hud_texture;
    assert(!valid_cache(frame, previous));

    assert(sizeof(frame_packet) == 416);
    assert(sizeof(pose_packet) == 264);
    assert(sizeof(cache_packet) == 80);
    std::cout << "PASS: token exhaustion, cancellation ABA, session identity, fence retirement, quarantine, cache/pan/zoom mapping, malformed projection and ABI\n";
}
