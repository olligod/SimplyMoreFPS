#include "../ownership.h"
#include "../mac_backend.h"
#include "../core.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    mac::missed_callbacks<2> lost;
    lost.record(0, 41);
    assert(lost.contains(0, 41));
    assert(!lost.contains(0, 42));
    lost.record(0, 42);
    lost.record(0, 41);
    assert(lost.contains(0, 42));
    lost.record(1, 11);
    assert(lost.contains(0, 42) && lost.contains(1, 11));
    lost.record(3, 999);
    lost.record(0, -1);
    assert(lost.contains(0, 42));

    // A delayed dispatch must never erase the replacement dispatch's loss.
    std::thread earlier([&] {
        for (int i = 1; i < 50000; ++i) {
            lost.record(0, i);
        }
    });
    for (int i = 50000; i < 100000; ++i) {
        lost.record(0, i);
    }
    earlier.join();
    assert(lost.contains(0, 99999));

    mac::texture_budget memory;
    memory.add(1, 1920, 1080);
    memory.add(1, 1920, 1080);
    assert(memory.bytes == 1920ull * 1080 * 4 && memory.count == 1);
    assert(memory.allows(1920ull * 1080 * 4, 1920ull * 1080 * 16));
    memory.add(1, 1280, 720);
    assert(!memory.valid && !memory.allows(0, 0));

    memory = {};
    for (unsigned i = 0; i < memory.items.size(); ++i) {
        memory.add(i + 1, 1, 1);
    }
    assert(memory.valid && memory.count == memory.items.size());
    memory.add(memory.items.size() + 1, 1, 1);
    assert(!memory.valid);

    memory = {};
    memory.add(1, 1920, 1080, 8);
    memory.add(1, 1920, 1080, 8);
    assert(memory.valid && memory.bytes == 1920ull * 1080 * 8);
    memory.add(1, 1920, 1080, 4);
    assert(!memory.valid);

    memory = {};
    memory.add(1, 16384, 16384);
    assert(!memory.allows(0, 0));

    memory = {};
    assert(memory.allows(memory.limit, 0));
    assert(!memory.allows(memory.limit, 1));
    assert(!memory.allows(UINT64_MAX, 0));
    assert(memory.allows(memory.scene_limit, 0, memory.scene_limit));
    assert(!memory.allows(memory.scene_limit, 1, memory.scene_limit));
    assert(!memory.allows(UINT64_MAX, 0, memory.scene_limit));
    assert(memory.allows_scene_packet(memory.scene_packet_limit));
    assert(!memory.allows_scene_packet(memory.scene_packet_limit + 1));
    assert(memory.allows(memory.scene_packet_limit * 2, memory.scene_display_reserve, memory.scene_limit));
    assert(!memory.allows(memory.scene_packet_limit * 2 + 1, memory.scene_display_reserve, memory.scene_limit));

    // Two cache families and three live captures fit during a scene replacement.
    memory.add(1, 2048, 2048, 8);
    assert(memory.allows(1200ull * 1024 * 1024, 72ull * 1024 * 1024, memory.scene_limit));
    assert(!memory.allows(1200ull * 1024 * 1024, 72ull * 1024 * 1024));

    memory = {};
    const uint64_t scene_static = 413ull * 1024 * 1024;
    const uint64_t live_4k = 3840ull * 2160 * 64;
    const uint64_t display_4k = 3840ull * 2160 * 32 + 8ull * 1024 * 1024;
    assert(memory.allows_scene_packet(scene_static + live_4k));
    assert(memory.allows((scene_static + live_4k) * 2, memory.scene_display_reserve, memory.scene_limit));
    assert(memory.allows(scene_static * 2 + live_4k * 3, display_4k, memory.scene_limit));
    assert(!memory.allows(scene_static * 2 + live_4k * 4, display_4k, memory.scene_limit));

    for (int bits = 0; bits < 8; ++bits) {
        assert(mac::source_may_retire(bits & 1, bits & 2, bits & 4) == (bits == 1));
    }

    assert(!mac::original_behind_overlay(1, 2, 100, 50));
    assert(!mac::original_behind_overlay(1, 1, 50, 50));
    assert(!mac::original_behind_overlay(1, 1, 50, 0));
    assert(mac::original_behind_overlay(1, 1, 51, 50));
    assert(!mac::acquired_after_show(false, 100, 50));
    assert(!mac::acquired_after_show(true, 100, 0));
    assert(!mac::acquired_after_show(true, 49, 50));
    assert(!mac::acquired_after_show(true, 50, 50));
    assert(mac::acquired_after_show(true, 51, 50));

    // An older complete image must not be represented by a map-less packet.
    session_frame frame{};
    frame.flags = 4;
    assert(mac::valid_absent_world(frame));
    frame.pose.frame_id = 1;
    assert(!mac::valid_absent_world(frame));

    mac::affine a{2, 0, 35, 0, -3, 91};
    mac::affine b;
    assert(mac::invert_affine(a, b));
    auto c = mac::multiply_affine(a, b);
    assert(std::abs(c.a - 1) < 1e-12 && std::abs(c.e - 1) < 1e-12 && std::abs(c.c) < 1e-12 && std::abs(c.f) < 1e-12);

    std::cout << "PASS: ABI, concurrent missed dispatches, immutable budget, source retirement, presented frame ordering and affine inversion\n";
}
