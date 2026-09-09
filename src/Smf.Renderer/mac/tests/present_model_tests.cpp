#include "../present_status.h"
#include "../native_target.h"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <limits>

using namespace present_observer;

session_native_frame make_marker(uint64_t frame = 100, uint64_t generation = 9, uint64_t restore = 0) {
    return {64, 1, 7, restore, frame, 8, generation, 1280, 720, 0, 0};
}

void queue(present_model& model, uint64_t buffer) {
    assert(model.begin_queue(buffer, false, 0));
    model.end_queue(buffer, true, 2);
}

// A source EOF and a full copy into an acquired drawable, each on its own buffer.
struct fixture {
    present_model model{7};
    uint64_t source = 0;
    uint64_t final = 0;
    uint64_t drawable = 0;

    fixture(bool enqueue = true, bool exact = true, uint64_t generation = 9, uint64_t restore = 0) {
        source = model.register_buffer(1001, true, 0);
        assert(source);
        assert(model.eof(source, 50, make_marker(100, generation, restore)));
        if (enqueue) queue(model, source);

        final = model.register_buffer(1002, true, 0);
        assert(final);
        drawable = model.acquire(1003, 51, 1280, 720, 80, 1000, 0xdecaf);
        assert(drawable);

        bool copied = model.full_copy(final, 50, 51, drawable, 1280, 720, 80, exact);
        assert(copied == exact);
        if (enqueue) queue(model, final);
    }

    void done(bool source_good = true, bool final_good = true) {
        model.complete(source, 1001, source_good);
        model.complete(final, 1002, final_good);
    }

    void display(double time = 1.0) {
        model.present(drawable, 1003, final);
        model.presented(drawable, 1003, time, 1100);
    }
};

int main() {
    // Resume semantics trust Unity's final scaled rendering, not shader/source
    // pixel equality. Acquisition and target work must follow the fresh EOF.
    for (unsigned failure = 0; failure < 8; ++failure) {
        present_model model(7);
        model.set_geometry(1920, 1080, 80);
        auto a = model.register_buffer(1, true, 0);
        auto b = model.register_buffer(2, true, 0);
        uint64_t d = 0;
        if (failure == 1) d = model.acquire(3, 51, 1920, 1080, 80, 1000); // old drawable
        if (failure == 2) queue(model, b); // created later is not enqueued later

        assert(model.eof(a, 50, make_marker()));
        queue(model, a);
        if (!d) d = model.acquire(3, 51, 1920, 1080, 80, 1000);

        if (failure != 3) {
            assert(model.target_write(b, 51, d));
            for (int i = 0; i < 64; ++i) {
                assert(model.target_write(b, 51, d));
            }
        }

        if (failure != 2) queue(model, b);
        model.present(d, 3, failure == 4 ? a : b);
        if (failure == 5) model.set_geometry(3024, 1898, 80);
        model.presented(d, 3, failure == 6 ? 0 : 1, 1100);
        model.complete(a, 1, failure != 7);
        model.complete(b, 2, true);

        presented_frame resumed{};
        assert(model.poll(resumed) == (failure == 0));
        if (!failure) assert(resumed.frame == 100 && resumed.content == 8 && resumed.generation == 9);
    }

    // The same command buffer uses the actual encoder operation order.
    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        auto d = model.acquire(3, 51, 1920, 1080, 80, 1000);
        assert(model.target_write(a, 51, d));
        model.present(d, 3, a, true);
        model.present(d, 3, a);
        queue(model, a);
        model.complete(a, 1, true);
        model.presented(d, 3, 1, 1100);
        presented_frame resumed{};
        assert(model.poll(resumed));
    }

    // A geometry change requires a new marker; an old EOF cannot cross the epoch.
    {
        present_model model(7);
        model.set_geometry(1280, 720, 80);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);
        model.set_geometry(1920, 1080, 80);
        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1920, 1080, 80, 1000);
        assert(model.target_write(b, 51, d));
        queue(model, b);
        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        model.complete(a, 1, true);
        model.complete(b, 2, true);
        presented_frame resumed{};
        assert(!model.poll(resumed));
    }

    // Sustained scaled resume frames stay bounded with missing callbacks.
    for (bool callback : {false, true}) {
        present_model model(7);
        model.set_geometry(1920, 1080, 80);

        for (uint64_t frame = 1; frame <= 2000; ++frame) {
            auto a = model.register_buffer(frame * 10 + 1, true, 0);
            assert(a && model.eof(a, 50, make_marker(frame)));
            queue(model, a);

            auto b = model.register_buffer(frame * 10 + 2, true, 0);
            auto d = model.acquire(frame * 10 + 3, 51, 1920, 1080, 80, frame * 10);
            assert(b && d && model.target_write(b, 51, d));
            queue(model, b);

            model.complete(a, frame * 10 + 1, true);
            model.complete(b, frame * 10 + 2, true);
            model.present(d, frame * 10 + 3, b);
            if (callback) model.presented(d, frame * 10 + 3, 1, frame * 10 + 1);

            presented_frame resumed{};
            assert(model.poll(resumed) == callback);
            assert(model.fault == fault_kind::none && model.live_buffers() < 32);
        }
    }

    static_assert(sizeof(present_status) == 192 && offsetof(present_status, counters) == 56 && offsetof(present_status, source_thread) == 184, "separate status ABI");
    presented_frame w{};

    {
        fixture f;
        assert(!f.model.poll(w));
        f.done();
        assert(!f.model.poll(w));
        f.model.present(f.drawable, 1003, f.final);
        assert(!f.model.poll(w));
        f.model.presented(f.drawable, 1003, 2.0, 1200);
        assert(f.model.poll(w));
        assert(w.session == 7 && w.frame == 100 && w.content == 8 && w.generation == 9 && !w.restore);
        assert(w.drawable == 0xdecaf && w.acquired_ns == 1000 && w.presented_ns == 1200 && w.serial);
        assert(!f.model.poll(w));
    }

    {
        fixture f;
        f.display();
        assert(!f.model.poll(w));
        f.model.complete(f.final, 1002, true);
        assert(!f.model.poll(w));
        f.model.complete(f.source, 1001, true);
        assert(f.model.poll(w));
    }

    {
        fixture f(true, true, 0, 71);
        f.done();
        f.display();
        assert(f.model.poll(w));
        assert(w.generation == 0 && w.restore == 71);
    }

    // Status 4 does not establish queue order.
    {
        fixture f(false);
        f.done();
        f.display();
        assert(!f.model.poll(w));
    }

    {
        fixture f;
        f.model.present(f.drawable, 1003, f.source);
        f.model.presented(f.drawable, 1003, 1, 1100);
        f.done();
        assert(!f.model.poll(w));
    }

    // No actual scheduled present.
    {
        fixture f;
        f.done();
        f.model.presented(f.drawable, 1003, 1, 1100);
        assert(!f.model.poll(w));
    }

    for (double time : {0., -1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        fixture f;
        f.done();
        f.display(time);
        assert(!f.model.poll(w));
        assert(f.model.counters.presented_zero == 1);
        assert(f.model.live_drawables() == 0);
    }

    {
        fixture f;
        f.done();
        f.model.present(f.drawable, 1003, f.final);
        f.model.presented(f.drawable, 1003, 1, 999);
        assert(!f.model.poll(w));
    }

    {
        fixture f;
        f.display();
        f.done(false);
        assert(!f.model.poll(w));
        assert(f.model.fault == fault_kind::identity);
    }

    {
        fixture f;
        f.display();
        f.done(true, false);
        assert(!f.model.poll(w));
    }

    {
        fixture f(true, false);
        f.done();
        f.display();
        assert(!f.model.poll(w));
    }

    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);

        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
        assert(!model.full_copy(b, mutation == 0 ? 99 : 50, mutation == 1 ? 52 : 51, d, mutation == 2 ? 1279 : 1280,
                                mutation == 3 ? 719 : 720, mutation == 4 ? 81 : 80, true));
        queue(model, b);

        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        model.complete(a, 1, true);
        model.complete(b, 2, true);
        assert(!model.poll(w));
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        assert(model.write(a, 50));
        queue(model, a);

        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(b, 50, 51, d, 1280, 720, 80, true));
        queue(model, b);

        model.complete(a, 1, true);
        model.complete(b, 2, true);
        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        assert(!model.poll(w));
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);

        auto writer = model.register_buffer(4, true, 0);
        assert(model.write(writer, 50));
        queue(model, writer);

        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(b, 50, 51, d, 1280, 720, 80, true));
        queue(model, b);

        model.complete(a, 1, true);
        model.complete(writer, 4, true);
        model.complete(b, 2, true);
        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        assert(!model.poll(w));
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);

        auto unknown = model.register_buffer(4, true, 0);
        assert(model.write(unknown, 50));

        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(b, 50, 51, d, 1280, 720, 80, true));
        queue(model, b);

        model.complete(a, 1, true);
        model.complete(b, 2, true);
        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        assert(!model.poll(w));
        queue(model, unknown); // an actual later enqueue disambiguates the future source write
        assert(model.poll(w));
        model.complete(unknown, 4, true);
    }

    // An observed pre-watch write enqueued after the EOF invalidates that source.
    {
        present_model model(7);
        auto older = model.register_buffer(4, true, 0);
        assert(model.write(older, 50));
        assert(!model.watched(50));

        auto a = model.register_buffer(1, true, 0);
        assert(model.eof(a, 50, make_marker()));
        queue(model, a);
        queue(model, older);

        auto b = model.register_buffer(2, true, 0);
        auto d = model.acquire(3, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(b, 50, 51, d, 1280, 720, 80, true));
        queue(model, b);

        model.complete(a, 1, true);
        model.complete(older, 4, true);
        model.complete(b, 2, true);
        model.present(d, 3, b);
        model.presented(d, 3, 1, 1100);
        assert(!model.poll(w));
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        auto b = model.register_buffer(2, true, 0);

        assert(model.begin_queue(a, true, 0));
        assert(model.begin_queue(b, true, 0));
        model.end_queue(a, true, 1);
        model.end_queue(b, true, 1);

        assert(model.fault == fault_kind::none);
        assert(!present_model::before(*model.buffer(a), *model.buffer(b)));
        assert(!present_model::before(*model.buffer(b), *model.buffer(a)));
    }

    // Unrelated source-copy work on Unity's queue can overlap the marker call.
    // Only the marker-to-final order is required by the resume contract.
    for (bool copy_ends_first : {false, true}) {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        auto copy = model.register_buffer(2, true, 0);
        assert(model.eof(a, 50, make_marker()));
        assert(model.write(copy, 70));

        assert(model.begin_queue(a, false, 0));
        assert(model.begin_queue(copy, false, 0));
        model.end_queue(copy_ends_first ? copy : a, true, 2);
        model.end_queue(copy_ends_first ? a : copy, true, 2);

        auto b = model.register_buffer(3, true, 0);
        auto d = model.acquire(4, 51, 1920, 1080, 80, 1000);
        assert(model.target_write(b, 51, d));
        queue(model, b);

        model.present(d, 4, b);
        model.complete(a, 1, true);
        model.complete(copy, 2, true);
        model.complete(b, 3, true);
        model.presented(d, 4, 1, 1100);
        assert(model.poll(w) && w.frame == 100 && model.fault == fault_kind::none && !model.counters.queue_conflicts);
    }

    // Overlapping marker/final calls remain unprovable despite positive GPU
    // and presentation receipts. A subsequent disjoint frame can recover.
    for (bool final_ends_first : {false, true}) {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        auto b = model.register_buffer(2, true, 0);
        assert(model.eof(a, 50, make_marker()));
        auto d = model.acquire(3, 51, 1920, 1080, 80, 1000);
        assert(model.target_write(b, 51, d));

        assert(model.begin_queue(a, false, 0));
        assert(model.begin_queue(b, false, 0));
        model.end_queue(final_ends_first ? b : a, true, 2);
        model.end_queue(final_ends_first ? a : b, true, 2);

        model.present(d, 3, b);
        model.complete(a, 1, true);
        model.complete(b, 2, true);
        model.presented(d, 3, 1, 1100);
        assert(!model.poll(w) && model.fault == fault_kind::none);

        auto next = model.register_buffer(4, true, 0);
        assert(model.eof(next, 50, make_marker(101)));
        queue(model, next);
        auto final = model.register_buffer(5, true, 0);
        auto fresh = model.acquire(6, 52, 1920, 1080, 80, 2000);
        assert(model.target_write(final, 52, fresh));
        queue(model, final);

        model.present(fresh, 6, final);
        model.complete(next, 4, true);
        model.complete(final, 5, true);
        model.presented(fresh, 6, 2, 2100);
        assert(model.poll(w) && w.frame == 101 && model.fault == fault_kind::none);
    }

    // A fast completion inside commit must not retire its open wrapper epoch.
    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.begin_queue(a, false, 0));
        model.complete(a, 1, true);

        model.sweep();
        assert(model.buffer(a) && model.buffer(a)->queue_call);
        model.end_queue(a, true, 4);
        assert(model.buffer(a) && model.buffer(a)->enqueue_end);
        model.sweep();
        assert(!model.buffer(a) && model.fault == fault_kind::none);
    }

    // Ambiguous writes to the tracked source still block the exact-copy route.
    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        auto writer = model.register_buffer(2, true, 0);
        assert(model.eof(a, 50, make_marker()));
        assert(model.write(writer, 50));

        assert(model.begin_queue(a, false, 0));
        assert(model.begin_queue(writer, false, 0));
        model.end_queue(a, true, 2);
        model.end_queue(writer, true, 2);

        auto b = model.register_buffer(3, true, 0);
        auto d = model.acquire(4, 51, 1280, 720, 80, 1000);
        assert(model.full_copy(b, 50, 51, d, 1280, 720, 80, true));
        queue(model, b);

        model.present(d, 4, b);
        model.complete(a, 1, true);
        model.complete(writer, 2, true);
        model.complete(b, 3, true);
        model.presented(d, 4, 1, 1100);

        assert(!model.poll(w) && model.fault == fault_kind::none);
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.begin_queue(a, false, 0));
        assert(!model.begin_queue(a, false, 0));
        assert(model.fault == fault_kind::queue_conflict);
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.begin_queue(a, false, 0));
        model.end_queue(a, true, 0);
        assert(model.fault == fault_kind::queue_conflict);
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        queue(model, a);
        assert(!model.begin_queue(a, false, 1));
        assert(model.fault == fault_kind::none);
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.register_buffer(1, false, 0) == a);
        assert(!model.register_buffer(1, true, 0));
        assert(model.fault == fault_kind::identity);
    }

    {
        fixture f;
        f.done();
        f.display();
        f.model.stop();
        assert(!f.model.poll(w));
        assert(f.model.gpu_idle());
        assert(!f.model.live_buffers() && !f.model.live_drawables());
    }

    {
        fixture f;
        f.model.stop();
        assert(!f.model.gpu_idle());
        f.done();
        assert(f.model.gpu_idle());
        assert(!f.model.poll(w));
    }

    {
        fixture f;
        f.done();
        auto old = f.drawable;
        auto replacement = f.model.acquire(1003, 51, 1280, 720, 80, 2000);
        assert(replacement && replacement != old);
        f.model.presented(old, 1003, 1, 2100);
        assert(f.model.counters.stale_callbacks == 1);
        assert(!f.model.poll(w));
    }

    {
        fixture f;
        assert(!f.model.acquire(1003, 51, 1280, 720, 80, 2000));
        assert(f.model.fault == fault_kind::identity);
    }

    {
        present_model model(7);
        for (uint64_t i = 1; i <= present_model::buffer_limit; ++i) {
            assert(model.register_buffer(i, true, 0));
        }
        assert(!model.register_buffer(999, true, 0));
        assert(model.fault == fault_kind::pool);
    }

    // Sustained positive, missing, and zero callbacks reuse fixed pools. No
    // completed-history or absent-presented leak may stall a long-running game.
    for (unsigned mode = 0; mode < 3; ++mode) {
        present_model model(7);
        uint64_t previous = 0;

        for (uint64_t frame = 1; frame <= 2000; ++frame) {
            auto source = model.register_buffer(frame * 10 + 1, true, 0);
            assert(source);
            assert(model.eof(source, 50, make_marker(frame)));
            queue(model, source);

            auto final = model.register_buffer(frame * 10 + 2, true, 0);
            assert(final);
            auto d = model.acquire(frame * 10 + 3, 51, 1280, 720, 80, frame * 100);
            assert(d);
            assert(model.full_copy(final, 50, 51, d, 1280, 720, 80, true));
            queue(model, final);

            model.complete(source, frame * 10 + 1, true);
            model.complete(final, frame * 10 + 2, true);
            model.present(d, frame * 10 + 3, final);
            if (mode != 1) model.presented(d, frame * 10 + 3, mode == 2 ? 0 : 1, frame * 100 + 1);

            if (mode == 0) {
                assert(model.poll(w));
                assert(w.frame == frame && w.serial > previous);
                previous = w.serial;
            } else {
                assert(!model.poll(w));
            }

            assert(model.live_buffers() < 32 && model.live_drawables() <= 8 && model.fault == fault_kind::none);
        }

        model.stop();
        assert(model.gpu_idle() && !model.live_buffers() && !model.live_drawables());
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        assert(model.register_buffer(1, false, 0) == a);
        assert(model.begin_queue(a, true, 0));
        model.end_queue(a, true, 1);
        assert(model.register_buffer(1, false, 1) == a);
        assert(!model.register_buffer(1, false, 0));
        assert(model.fault == fault_kind::identity && !model.buffer(a)->completed);
    }

    {
        present_model model(7);
        auto a = model.register_buffer(1, true, 0);
        queue(model, a);
        assert(!model.register_buffer(1, true, 0));
        assert(model.fault == fault_kind::identity && !model.buffer(a)->completed);
    }

    // Completed EOF history remains required for a pending old drawable even
    // while the same Metal buffer object starts a new submission.
    for (bool fresh : {false, true}) {
        fixture f;
        f.done();
        assert(f.model.buffer(f.source) && f.model.buffer(f.source)->completed);

        auto reused = f.model.register_buffer(1001, fresh, 0);
        assert(reused && reused != f.source);
        assert(f.model.buffer(f.source) && f.model.buffer(f.source)->completed && !f.model.buffer(reused)->completed);
        assert(f.model.eof(reused, 50, make_marker(101)));

        auto stale = f.model.counters.stale_callbacks;
        f.model.complete(f.source, 1001, true);
        assert(f.model.counters.stale_callbacks == stale + 1 && !f.model.buffer(reused)->completed && f.model.fault == fault_kind::none);
        f.display();
        assert(f.model.poll(w) && w.frame == 100); // the old pending drawable keeps the old EOF

        queue(f.model, reused);
        auto final = f.model.register_buffer(1002, true, 0);
        auto drawable = f.model.acquire(1003, 51, 1280, 720, 80, 2000);
        assert(final && drawable);
        assert(f.model.full_copy(final, 50, 51, drawable, 1280, 720, 80, true));
        queue(f.model, final);

        f.model.complete(reused, 1001, true);
        f.model.complete(final, 1002, true);
        f.model.present(drawable, 1003, final);
        f.model.presented(drawable, 1003, 1, 2100);
        assert(f.model.poll(w) && w.frame == 101 && f.model.fault == fault_kind::none);
    }

    for (uint32_t status : {1u, 2u, 4u}) {
        fixture f;
        f.done();
        assert(!f.model.register_buffer(1001, false, status));
        assert(f.model.buffer(f.source)->completed);
    }

    {
        present_model model(7);
        uint64_t old = 0;

        for (uint64_t frame = 1; frame <= 6000; ++frame) {
            auto source = model.register_buffer(1001, true, 0);
            assert(source && source != old);
            assert(model.eof(source, 50, make_marker(frame)));
            queue(model, source);

            auto final = model.register_buffer(1002, true, 0);
            auto drawable = model.acquire(1003, 51, 1280, 720, 80, frame * 10);
            assert(final && drawable);
            assert(model.full_copy(final, 50, 51, drawable, 1280, 720, 80, true));
            queue(model, final);

            if (old) {
                auto stale = model.counters.stale_callbacks;
                model.complete(old, 1001, true);
                assert(model.counters.stale_callbacks == stale + 1 && !model.buffer(source)->completed);
            }

            model.complete(source, 1001, true);
            model.complete(final, 1002, true);
            model.present(drawable, 1003, final);
            model.presented(drawable, 1003, 1, frame * 10 + 1);
            assert(model.poll(w) && w.frame == frame && model.fault == fault_kind::none && model.live_buffers() < 8);
            old = source;
        }

        model.stop();
        assert(model.gpu_idle());
    }

    {
        using namespace mac;
        native_target_stage stage;
        mac_source_target target{64, 1, 7, 8, 0, 100, 0x1000, 1280, 720, 0, 0};
        auto marker = make_marker(100, 0, 71);
        assert(valid_native_target(target) && !valid_source_target(target));
        assert(stage.stage(target, 99) == 0 && stage.matches(marker));

        auto beginning = marker;
        beginning.flags = 1;
        assert(!stage.matches(beginning));

        for (unsigned field = 0; field < 6; ++field) {
            auto bad = marker;
            if (field == 0) ++bad.session;
            if (field == 1) ++bad.content_revision;
            if (field == 2) ++bad.generation;
            if (field == 3) ++bad.source_frame;
            if (field == 4) ++bad.width;
            if (field == 5) ++bad.height;
            assert(!stage.matches(bad));
        }

        assert(stage.stage(target, 100) == 1);
        auto conflict = target;
        conflict.native_render_buffer++;
        assert(stage.stage(conflict, 99) == -201);

        stage.clear();
        target.content_revision = 0;
        marker.content_revision = 0;
        assert(stage.stage(target, 0) == 0 && stage.matches(marker));
    }

    std::cout << "PASS present source/copy/queue/completion/presentation model, negative routes, generation0 restore, bounded 6000-frame recycling and late callbacks\n";
}
