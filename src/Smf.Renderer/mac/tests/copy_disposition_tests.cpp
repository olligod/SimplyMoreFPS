#include "../copy_disposition.h"
#include "../present_model.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace mac;
    using namespace present_observer;

    // Factory/encoder/encode/end/handler-registration failures all occur
    // before commit. Disposal never retires a still-running producer.
    for (int boundary = 0; boundary < 5; ++boundary) {
        copy_disposition state;
        assert(state.discard());
        assert(!state.retired(false, false));
        assert(state.retired(true, false));
        assert(!state.usable(true, true));
        assert(!state.usable(false, true));
    }

    {
        copy_disposition state;
        state.begin_commit();
        assert(!state.discard());
        assert(!state.retired(true, false));
        assert(!state.retired(false, true));
        assert(state.retired(true, true) && state.usable(true, true));
        assert(!state.usable(false, true));
    }

    {
        present_model model(7);
        auto b = model.register_buffer(10, true, 0);
        assert(model.write(b, 90));
        assert(!model.discard_unsubmitted_copy(b, 11));
        assert(model.discard_unsubmitted_copy(b, 10));
        assert(model.gpu_idle() && !model.live_buffers());

        auto newer = model.register_buffer(10, true, 0);
        assert(newer && newer != b);
        model.complete(b, 10, true);
        assert(!model.buffer(newer)->completed);
    }

    // Reuse of the same Objective-C identity is a fresh Unity submission.
    // Stale SMF disposal must not erase its writes or EOF evidence.
    {
        present_model model(7);
        auto old = model.register_buffer(10, true, 0);
        assert(model.write(old, 90));
        assert(model.discard_unsubmitted_copy(old, 10));

        auto unity = model.register_buffer(10, true, 0);
        assert(unity != old);
        session_native_frame marker{64, 1, 7, 0, 100, 8, 9, 1280, 720, 0, 0};
        assert(model.write(unity, 50));
        assert(model.eof(unity, 50, marker));
        assert(!model.discard_unsubmitted_copy(old, 10));
        assert(!model.discard_unsubmitted_copy(unity, 10));
        assert(model.buffer(unity) && model.buffer(unity)->count == 2);
        assert(model.begin_queue(unity, false, 0));
        model.end_queue(unity, true, 2);

        auto final = model.register_buffer(11, true, 0);
        auto drawable = model.acquire(12, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(final, 50, 51, drawable, 1280, 720, 80, true));
        assert(model.begin_queue(final, false, 0));
        model.end_queue(final, true, 2);

        model.complete(old, 10, true);
        assert(!model.buffer(unity)->completed);
        model.complete(unity, 10, true);
        model.complete(final, 11, true);
        model.present(drawable, 12, final);
        model.presented(drawable, 12, 1, 1100);

        presented_frame frame{};
        assert(model.poll(frame) && frame.frame == 100);
    }

    for (int kind = 0; kind < 4; ++kind) {
        present_model model(7);
        auto source = model.register_buffer(1, true, 0);
        session_native_frame marker{64, 1, 7, 0, 100, 8, 9, 1280, 720, 0, 0};
        assert(model.eof(source, 50, marker));
        auto copy = model.register_buffer(2, true, 0);

        if (kind == 0) {
            assert(model.write(copy, 50)); // watched overwrite cannot vanish
        }
        if (kind == 1) {
            assert(model.eof(copy, 51, marker));
        }
        if (kind == 2) {
            assert(model.begin_queue(copy, false, 0));
            model.end_queue(copy, true, 2);
        }
        if (kind == 3) {
            auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
            assert(model.full_copy(copy, 50, 51, d, 1280, 720, 80, true));
        }

        assert(!model.discard_unsubmitted_copy(copy, 2));
    }

    // Private discarded writes do not remove source EOF or original-present
    // prerequisites. A later full copy still needs both GPU and present.
    {
        present_model model(7);
        auto source = model.register_buffer(1, true, 0);
        session_native_frame marker{64, 1, 7, 0, 100, 8, 9, 1280, 720, 0, 0};
        assert(model.eof(source, 50, marker));
        assert(model.begin_queue(source, false, 0));
        model.end_queue(source, true, 2);

        auto private_copy = model.register_buffer(2, true, 0);
        assert(model.write(private_copy, 90));
        assert(model.discard_unsubmitted_copy(private_copy, 2));

        auto final = model.register_buffer(3, true, 0);
        auto drawable = model.acquire(4, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(final, 50, 51, drawable, 1280, 720, 80, true));
        assert(model.begin_queue(final, false, 0));
        model.end_queue(final, true, 2);
        model.complete(source, 1, true);
        model.complete(final, 3, true);

        presented_frame frame{};
        assert(!model.poll(frame));
        model.present(drawable, 4, final);
        model.presented(drawable, 4, 1, 1100);
        assert(model.poll(frame) && frame.frame == 100);
    }

    std::cout << "PASS owned-copy precommit disposal, producer retention, ambiguous commit, exact discard and presented frame preservation\n";
}
