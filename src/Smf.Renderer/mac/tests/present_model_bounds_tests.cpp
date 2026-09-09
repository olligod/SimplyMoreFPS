#include "../present_model.h"
#include <cassert>
#include <iostream>

using namespace present_observer;

session_native_frame make_marker(uint64_t frame = 100) {
    return {64, 1, 7, 0, frame, 8, 9, 1280, 720, 0, 0};
}

void queue(present_model& model, uint64_t buffer) {
    assert(model.begin_queue(buffer, false, 0));
    model.end_queue(buffer, true, 2);
}

// Acquires a drawable and copies texture 50 into it on the given buffer.
uint64_t copy(present_model& model, uint64_t buffer, uint64_t object = 3, uint64_t texture = 51) {
    auto drawable = model.acquire(object, texture, 1280, 720, 80, 1000);
    assert(drawable);
    assert(model.full_copy(buffer, 50, texture, drawable, 1280, 720, 80, true));
    return drawable;
}

bool display(present_model& model, uint64_t buffer, uint64_t drawable, uint64_t object = 3, uint64_t frame = 100) {
    model.present(drawable, object, buffer);
    model.presented(drawable, object, 1, 1100);

    presented_frame found;
    bool ok = model.poll(found);
    if (ok) assert(found.frame == frame);
    return ok;
}

void flood(present_model& model, uint64_t buffer, uint64_t texture = 50) {
    for (int i = 0; i < 100; ++i) {
        assert(model.write(buffer, texture));
    }
}

int main() {
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        flood(model, b);
        assert(model.buffer(b)->count == 1 && model.counters.source_writes == 100);
        assert(model.eof(b, 50, make_marker()));
        auto d = copy(model, b);
        queue(model, b);
        model.complete(b, 1, true);
        assert(display(model, b, d) && model.fault == fault_kind::none);
    }

    // Writes on both sides of the EOF stay separate: a post-EOF overwrite
    // invalidates the copy even though the same texture was written before.
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        flood(model, b);
        assert(model.eof(b, 50, make_marker()));
        flood(model, b);
        auto d = copy(model, b);
        assert(model.buffer(b)->count == 4);
        queue(model, b);
        model.complete(b, 1, true);
        assert(!display(model, b, d));
    }

    // A write after an old copy invalidates a later copy, not the old drawable.
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        assert(model.eof(b, 50, make_marker()));
        auto old = copy(model, b);
        flood(model, b);
        auto newer = copy(model, b, 4, 52);
        flood(model, b);
        assert(model.buffer(b)->count == 5);
        queue(model, b);
        model.complete(b, 1, true);
        assert(display(model, b, old));
        assert(!display(model, b, newer, 4));
    }

    // Replacing earlier writes with their latest order would wrongly accept
    // the middle copy here. Both write-only segments must survive.
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        assert(model.eof(b, 50, make_marker()));
        flood(model, b);
        auto d = copy(model, b);
        flood(model, b);
        queue(model, b);
        model.complete(b, 1, true);
        assert(!display(model, b, d));
    }

    // A later EOF establishes a new valid source after overwritten data.
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        assert(model.eof(b, 50, make_marker()));
        flood(model, b);
        assert(model.eof(b, 50, make_marker(101)));
        auto d = copy(model, b);
        queue(model, b);
        model.complete(b, 1, true);
        assert(display(model, b, d, 3, 101));
    }

    // A reused object starts a distinct submission; the old pending copy keeps its EOF.
    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        flood(model, a);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);

        auto b = model.register_buffer(2, true, 0);
        auto d = copy(model, b);
        queue(model, b);
        model.complete(a, 1, true);
        model.complete(b, 2, true);

        auto reused = model.register_buffer(1, true, 0);
        assert(reused != a);
        flood(model, reused);
        assert(model.eof(reused, 50, make_marker(101)));
        queue(model, reused);
        model.complete(reused, 1, true);
        assert(model.buffer(a) && model.buffer(a)->completed);
        assert(display(model, b, d));

        auto final = model.register_buffer(2, true, 0);
        auto next = copy(model, final);
        queue(model, final);
        model.complete(final, 2, true);
        assert(display(model, final, next, 3, 101));
    }

    // Distinct textures consume bounded records; duplicates at capacity are
    // still accepted, while the seventeenth distinct texture fails closed.
    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);

        for (uint64_t t = 1; t <= 16; ++t) {
            assert(model.write(b, t));
        }
        for (uint64_t t = 1; t <= 16; ++t) {
            flood(model, b, t);
        }

        assert(model.buffer(b)->count == 16 && model.counters.source_writes == 1616);
        assert(!model.write(b, 17) && model.fault == fault_kind::pool);
    }

    {
        present_model model(7);
        auto b = model.register_buffer(1, true, 0);
        for (int i = 0; i < 8; ++i) {
            flood(model, b);
            assert(model.eof(b, 50, make_marker(100 + i)));
        }

        assert(model.buffer(b)->count == 16);
        assert(!model.write(b, 50) && model.fault == fault_kind::pool);
    }

    // An overwrite from another buffer in between still disqualifies under floods.
    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);

        auto writer = model.register_buffer(4, true, 0);
        flood(model, writer);
        queue(model, writer);

        auto b = model.register_buffer(2, true, 0);
        auto d = copy(model, b);
        queue(model, b);

        model.complete(a, 1, true);
        model.complete(writer, 4, true);
        model.complete(b, 2, true);
        assert(!display(model, b, d));
    }

    {
        present_model model(7);
        for (uint64_t frame = 1; frame <= 2000; ++frame) {
            auto b = model.register_buffer(1, true, 0);
            assert(b);
            flood(model, b);
            assert(model.eof(b, 50, make_marker(frame)));
            auto d = copy(model, b);
            queue(model, b);
            model.complete(b, 1, true);
            assert(display(model, b, d, 3, frame));
            assert(model.live_buffers() < 8 && model.fault == fault_kind::none);
        }

        model.stop();
        assert(model.gpu_idle() && !model.live_buffers() && !model.live_drawables());
    }

    std::cout << "PASS write floods, EOF/copy boundaries, overwrite rejection, history, distinct bounds, 2000-frame reuse\n";
}
