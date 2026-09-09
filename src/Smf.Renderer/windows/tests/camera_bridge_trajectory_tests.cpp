// Drives the real bridge against the NativeAOT kernel DLL. No window, GPU or Unity.
#define SMF_BRIDGE_INTERNAL
#include "../camera_bridge.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

static void check(bool okay, const char* message) {
    if (!okay) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main(int argc, char** argv) {
    check(argc == 2, "kernel path argument");
    const auto path = std::filesystem::absolute(argv[1]).wstring();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    camera_bridge::worker_ready(GetCurrentThreadId(), frequency.QuadPart);

    check(smf_camera_bridge_init(path.c_str(), uint32_t(path.size())) == S_OK, "real kernel module path");
    check(smf_camera_bridge_revoke(1, 0) == S_OK, "epoch fence");

    smf_bridge_main m{};
    m.size = sizeof(m);
    m.version = 2;
    m.publication = 1;

    auto& s = m.state;
    s.version = 2;
    s.size = sizeof(s);
    s.epoch = 1;
    s.map_id = 0;
    s.flags = 1;
    s.x = 60;
    s.z = 70;
    s.root_size = 24;
    s.projection_half_height = 35.9591827;
    s.min_size = 11;
    s.max_size = 60;
    s.ui_scale = 1;
    s.pixel_width = 1280;
    s.pixel_height = 720;

    auto& c = m.settings;
    c.version = 2;
    c.size = sizeof(c);
    c.revision = 1;
    c.map_width = 150;
    c.map_height = 150;
    c.pixel_width = 1280;
    c.pixel_height = 720;
    c.ui_scale = 1;
    c.min_size = 11;
    c.max_size = 60;
    c.dolly_rate_keys = 50;
    c.dolly_rate_screen_edge = 35;
    c.speed_decay = .85;
    c.move_speed = 2;
    c.zoom_speed = 2.6;
    c.scroll_wheel_rate = .35;
    c.drag_sensitivity = 1.3;
    c.profile.present_mask = 1;
    c.profile.projection.mode = SMF_CAMERA_CURVE_POWER_RANGE;
    c.profile.projection.input_min = 11;
    c.profile.projection.input_max = 60;
    c.profile.projection.a = 2;
    c.profile.projection.b = 130;
    c.profile.projection.exponent = 1;

    m.bindings.version = 1;
    m.bindings.size = sizeof(m.bindings);
    m.bindings.revision = 1;

    m.trajectory = {1, 1, 0, smf_camera_bridge_now(), .4, 60, 70, 24, 90, 70, 20};

    auto publish = [&]() {
        check(smf_camera_bridge_publish(&m, sizeof(m)) == S_OK, "atomic main publication");
        ++m.publication;
    };

    auto step = [&]() {
        smf_bridge_desired d{};
        check(camera_bridge::worker_prepare(true, true, 640, 360, false, d) == S_OK, "worker prepares target");
        camera_bridge::worker_committed(S_OK);
        return d;
    };

    m.size = 2008;
    check(smf_camera_bridge_publish(&m, 2008) == E_INVALIDARG, "old bundle size rejected");
    m.size = sizeof(m);
    m.size = 2168;
    check(smf_camera_bridge_publish(&m, 2168) == E_INVALIDARG, "old bounds bundle size rejected");
    m.size = sizeof(m);

    c.size = 1752;
    check(smf_camera_bridge_publish(&m, sizeof(m)) == E_INVALIDARG, "old settings size rejected");
    c.size = sizeof(c);
    c.size = 1912;
    check(smf_camera_bridge_publish(&m, sizeof(m)) == E_INVALIDARG, "old bounds settings size rejected");
    c.size = sizeof(c);

    publish();
    const auto seed = step();
    check(seed.version == 2 && seed.size == 88 && seed.x == 60 && seed.root_size == 24 && seed.active_pan_id == 0,
        "unacknowledged initial seed does not execute trajectory");

    sleep_ms(80);
    const auto waiting = step();
    check(waiting.sequence == seed.sequence && waiting.x == seed.x && waiting.active_pan_id == 0,
        "idle waiting keeps unchanged seed and zero trajectory");

    s.applied_sequence = seed.sequence;
    s.flags = 3;
    publish();
    const auto moving = step();
    check(moving.x > 60 && moving.x < 90 && moving.active_pan_id == 1, "acknowledged trajectory catches up using native clock");
    check(std::abs(moving.projection_half_height - (2 + (moving.root_size - 11) * 128 / 49)) < 1e-10,
        "logical zoom maps to actual projection in desired packet");

    sleep_ms(420);
    const auto done = step();
    check(done.x == 90 && done.root_size == 20 && done.active_pan_id == 0 && done.finished_pan_id == 1 &&
        done.pan_flags == SMF_CAMERA_PAN_COMPLETED, "trajectory completes independently with completion ID");

    smf_bridge_desired observed{};
    check(smf_camera_bridge_desired(&observed, sizeof(observed)) == S_OK && observed.finished_pan_id == 1,
        "committed completion reaches main mailbox");

    s.flags = 0;
    s.projection_half_height = 42;
    s.root_size = 24;
    publish();

    const auto fallback = step();
    check(fallback.projection_half_height == 42 && fallback.root_size == 24 && !fallback.active_pan_id &&
        !fallback.finished_pan_id && !fallback.pan_flags, "external fallback keeps actual projection and clears trajectory IDs");

    // A raw game jump may exceed a newly published zoom cap. The seed must survive every
    // pre-ack step untouched; MOTION_BLOCKED alone would still let zoom/bounds clamp it.
    check(smf_camera_bridge_revoke(2, 0) == S_OK, "capped seed epoch fence");

    s.epoch = 2;
    s.applied_sequence = 0;
    s.flags = 1;
    s.root_size = 60;
    s.projection_half_height = 60;
    s.max_size = 40;
    c.revision++;
    c.max_size = 40;
    c.profile = {};
    c.bounds.maximum_size = 40;
    m.trajectory = {};
    publish();

    const auto capped_seed = step();
    check(capped_seed.root_size == 60 && capped_seed.projection_half_height == 60 && capped_seed.x == s.x && capped_seed.z == s.z,
        "above-cap raw source is preserved as initial seed");

    for (int i = 0; i < 3; i++) {
        sleep_ms(20);
        const auto idle = step();
        check(idle.sequence == capped_seed.sequence && idle.root_size == 60 && idle.projection_half_height == 60 &&
            idle.x == capped_seed.x && idle.z == capped_seed.z && !idle.active_pan_id && !idle.finished_pan_id && !idle.pan_flags,
            "clock-only pre-ack step preserves complete above-cap seed");
    }

    s.applied_sequence = capped_seed.sequence;
    s.flags = 3;
    publish();
    const auto constrained = step();
    check(constrained.root_size == 40 && constrained.projection_half_height == 40 && constrained.sequence > capped_seed.sequence,
        "geometry cap applies only after seed acknowledgement");

    camera_bridge::worker_removed();
    std::puts("PASS bridge against the real kernel: seed ack gate, atomic trajectory, completion and projection fallback");
}
