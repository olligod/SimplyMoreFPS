// Queue and policy checks only: no hooks, windows, Unity or GPU.
// Link with camera_control.cpp and user32.lib.
#define SMF_BRIDGE_INTERNAL
#include "../wheel_queue.h"
#include <cassert>
#include <initializer_list>
#include <thread>

using namespace camera_control;

int main() {
    smf_control_policy p{};
    p.size = sizeof(p);
    p.version = 1;
    p.epoch = 7;
    p.revision = 1;
    p.flags = 8;
    p.width = 1280;
    p.height = 720;
    p.ui_scale = 1.5;

    // A modifier observed with the wheel stays attached to that event, even when a
    // queued zoom key delays the wheel until the next step.
    assert(wheel_modifiers_allowed(8, wheel_modifiers(true, true, true)));
    assert(!wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(true, false, false)));
    assert(wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(false, true, true)));
    assert(!wheel_modifiers_allowed(8 | wheel_alt, wheel_modifiers(false, true, false)));
    assert(!wheel_modifiers_allowed(8 | wheel_shift, wheel_modifiers(false, false, true)));

    wheel_queue<2> reserved;
    wheel_event retained{};
    assert(reserved.push({2, 7, 120, 900, 350, wheel_modifiers(true, false, false)}));
    const auto released = wheel_modifiers(false, false, false);
    assert(wheel_modifiers_allowed(8 | wheel_control, released));
    assert(reserved.pop(retained) && !wheel_modifiers_allowed(8 | wheel_control, retained.modifiers));

    auto flags_policy = p;
    flags_policy.flags = 127;
    assert(publish_policy(flags_policy) == S_OK);
    ++flags_policy.revision;
    flags_policy.flags = 128;
    assert(FAILED(publish_policy(flags_policy)));

    assert(wheel_policy_allows(p, 7, false, 300, 300));
    assert(!wheel_policy_allows(p, 8, false, 300, 300)); // epoch changed
    assert(!wheel_policy_allows(p, 7, true, 300, 300)); // focus or motion lost
    assert(!wheel_policy_allows(p, 7, false, -1, 300));
    assert(!wheel_policy_allows(p, 7, false, 1280, 300));
    p.flags = 0;
    assert(!wheel_policy_allows(p, 7, false, 300, 300)); // not owned yet
    p.flags = 12;
    assert(!wheel_policy_allows(p, 7, false, 300, 300)); // text field, hot control or modal

    p.flags = 8;
    p.rect_count = 1;
    p.rects[0] = {150, 75, 450, 225}; // logical 100,50..300,150 at UI scale 1.5
    assert(!wheel_policy_allows(p, 7, false, 150, 75));
    assert(!wheel_policy_allows(p, 7, false, 449, 224));
    assert(wheel_policy_allows(p, 7, false, 450, 225));

    p.rect_count = 65;
    assert(!wheel_policy_allows(p, 7, false, 900, 350)); // rect overflow

    assert(wheel_unity_delta(120) == -3 && wheel_unity_delta(-120) == 3 && wheel_unity_delta(-30) == .75);

    wheel_queue<4> q;
    wheel_event e{};
    assert(q.push({2, 7, -120, 900, 350}));
    assert(q.push({2, 7, 120, 900, 350}));
    assert(q.push({2, 7, -30, 900, 350}));
    assert(q.push({2, 7, -30, 900, 350}));
    assert(!q.push({2, 7, 120, 900, 350}));
    assert(q.count() == 4);

    for (int delta : {-120, 120, -30, -30}) {
        assert(q.pop(e));
        assert(e.delta == delta);
    }
    assert(!q.pop(e));

    assert(wheel_event_matches({2, 7, 120, 0, 0}, 2, 7));
    assert(!wheel_event_matches({2, 7, 120, 0, 0}, 3, 7)); // stop/start must not replay
    assert(!wheel_event_matches({2, 7, 120, 0, 0}, 2, 8)); // new epoch

    // The managed impulse queue refuses every wheel delta; zoom-key flags still queue.
    smf_control_impulse impulse{sizeof(impulse), 1, 7, 1, 0, 3, 0, 0};
    assert(FAILED(queue(impulse)));
    impulse.wheel_delta = 0;
    impulse.flags = 1;
    assert(queue(impulse) == S_OK);
    clear();

    // Lock-free ring under a real producer/consumer pair.
    wheel_queue<32> concurrent;
    std::thread producer([&] {
        for (uint64_t i = 1; i <= 100000; i++) {
            wheel_event x{i, 7, int32_t(i), 1, 2};
            while (!concurrent.push(x)) std::this_thread::yield();
        }
    });

    for (uint64_t i = 1; i <= 100000; i++) {
        while (!concurrent.pop(e)) std::this_thread::yield();
        assert(e.generation == i && e.delta == int32_t(i) && e.epoch == 7 && e.x == 1 && e.y == 2);
    }

    producer.join();
    assert(concurrent.count() == 0);
}
