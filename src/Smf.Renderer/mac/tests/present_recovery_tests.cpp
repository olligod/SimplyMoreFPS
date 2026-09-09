#include "../present_recovery.h"
#include <cassert>
#include <iostream>
#include <memory>

using namespace present_observer;

session_command make_restore(uint64_t serial = 71, uint64_t frame = 100) {
    session_command c{};
    c.size = 88;
    c.version = 1;
    c.session = 7;
    c.serial = serial;
    c.generation = 9;
    c.content_revision = 8;
    c.after_frame = frame;
    c.operation = 6;

    return c;
}

session_native_frame make_marker(uint64_t frame = 101, uint64_t restore = 71) {
    return {64, 1, 7, restore, frame, 8, 0, 1280, 720, 0, 0};
}

void queue(present_model& model, uint64_t id) {
    assert(model.begin_queue(id, false, 0));
    model.end_queue(id, true, 2);
}

// One native frame: a source EOF, then a full copy into an acquired drawable.
struct test_frame {
    uint64_t source = 0;
    uint64_t final = 0;
    uint64_t drawable = 0;

    explicit test_frame(present_model& model, session_native_frame marker, bool overwritten = false, bool exact = true) {
        source = model.register_buffer(1, true, 0);
        assert(source);
        assert(model.eof(source, 50, marker));
        if (overwritten) assert(model.write(source, 50));
        queue(model, source);

        final = model.register_buffer(2, true, 0);
        assert(final);

        drawable = model.acquire(3, 51, 1280, 720, 80, 1000, 0x123);
        assert(drawable);
        assert(model.full_copy(final, 50, 51, drawable, 1280, 720, 80, exact) == exact);
        queue(model, final);
    }

    void complete(present_model& model, bool good = true) {
        model.complete(source, 1, good);
        model.complete(final, 2, good);
    }

    void presented(present_model& model, double time = 1) {
        model.present(drawable, 3, final);
        model.presented(drawable, 3, time, 1100);
    }
};

int main() {
    {
        recovery r;
        r.begin(make_restore());

        int removes = 0;
        int installs = 0;

        for (int n = 0; n < 100; ++n) {
            r.pump([&] { ++removes; return 1; }, [&] { ++installs; return 0; });
            assert(!r.allows(make_marker(), 8));
        }

        assert(removes == 100 && !installs && r.state() == recovery::removing);
        r.pump([&] { ++removes; return 0; }, [&] { ++installs; return 1; });
        assert(r.state() == recovery::installing);
        r.pump([&] { assert(false); return 0; }, [&] { ++installs; return 0; });
        assert(r.state() == recovery::observing);
        assert(r.allows(make_marker(), 8));
        assert(removes == 101 && installs == 2);
        r.pump([&] { assert(false); return 0; }, [&] { assert(false); return 0; });
    }

    for (bool remove_fails : {false, true}) {
        recovery r;
        r.begin(make_restore());
        r.pump([&] { return remove_fails ? -202 : 0; }, [&] { return -204; });
        assert(r.state() == recovery::failed && r.error() == (remove_fails ? -202 : -204));
        r.pump([] { assert(false); return 0; }, [] { assert(false); return 0; });
        assert(!r.allows(make_marker(), 8));
    }

    {
        recovery r;
        r.begin(make_restore());
        r.pump([] { return 0; }, [] { return 0; });

        auto good = make_marker();
        assert(r.allows(good, 8));

        for (int n = 0; n < 7; ++n) {
            auto stale = good;
            switch (n) {
            case 0: stale.session++; break;
            case 1: stale.restore_serial = 0; break;
            case 2: stale.restore_serial--; break;
            case 3: stale.generation = 9; break;
            case 4: stale.content_revision--; break;
            case 5: stale.source_frame = 100; break;
            case 6: stale.flags = 1; break;
            }
            assert(!r.allows(stale, 8));
        }

        r.begin(make_restore(72, 110));
        assert(!r.allows(good, 8));
        r.pump([] { return 0; }, [] { return 0; });
        assert(!r.allows(good, 8));
        assert(r.allows(make_marker(111, 72), 8));

        r.fail(-204);
        assert(!r.allows(make_marker(112, 72), 8));
    }

    // A visible session fails with old GPU and presentation callbacks still
    // outstanding. Recovery cannot install while those GPU leases are live,
    // and the old callbacks keep the old model even when numeric ids repeat.
    for (auto fault : {fault_kind::pool, fault_kind::unknown_route, fault_kind::class_conflict, fault_kind::queue_conflict, fault_kind::identity}) {
        recovery r;
        auto old = std::make_shared<present_model>(7);
        test_frame prior(*old, make_marker(90, 0));
        old->fail(fault);
        old->stop();

        auto late_complete = [old, prior] {
            old->complete(prior.source, 1, true);
            old->complete(prior.final, 2, true);
        };
        auto late_presented = [old, prior] {
            old->present(prior.drawable, 3, prior.final);
            old->presented(prior.drawable, 3, 1, 1100);
        };

        r.begin(make_restore());
        std::shared_ptr<present_model> fresh;
        int installs = 0;
        auto remove = [&] { return old->gpu_idle() ? 0 : 1; };
        auto install = [&] {
            ++installs;
            fresh = std::make_shared<present_model>(7);
            return 0;
        };

        r.pump(remove, install);
        assert(!fresh && !installs);

        late_complete();
        r.pump(remove, install);
        assert(r.state() == recovery::observing && fresh && installs == 1);

        test_frame next(*fresh, make_marker());
        assert(next.source == prior.source && next.final == prior.final);

        late_complete();
        late_presented();
        presented_frame found{};
        assert(fresh->counters.gpu_completed == 0 && !fresh->poll(found));
        assert(!old->poll(found));

        next.complete(*fresh);
        assert(!fresh->poll(found));
        next.presented(*fresh);
        assert(fresh->poll(found) && r.allows(found, 8));
        assert(old->fault == fault);
    }

    // Recovery does not manufacture evidence when the new route is unsupported,
    // overwritten, partial, unpresented or GPU-faulted.
    for (int n = 0; n < 5; ++n) {
        recovery r;
        r.begin(make_restore());
        r.pump([] { return 0; }, [] { return 0; });

        present_model fresh(7);
        test_frame current(fresh, make_marker(), n == 1, n != 2);
        current.complete(fresh, n != 3);
        current.presented(fresh, n == 0 ? 0 : 1);
        if (n == 4) fresh.fail(fault_kind::unknown_route);

        presented_frame found{};
        assert(!fresh.poll(found));
    }

    {
        recovery r;
        r.begin(make_restore());
        r.pump([] { return 0; }, [] { return 0; });

        presented_frame good{7, 101, 0, 8, 71, 3, 99, 1000, 1100};
        assert(r.allows(good, 8));

        for (int n = 0; n < 6; ++n) {
            auto bad = good;
            switch (n) {
            case 0: bad.session++; break;
            case 1: bad.restore--; break;
            case 2: bad.generation = 9; break;
            case 3: bad.content--; break;
            case 4: bad.frame = 100; break;
            case 5: bad.restore = 0; break;
            }
            assert(!r.allows(bad, 8));
        }
    }

    {
        recovery r;
        r.begin(make_restore());
        r.pump([] { return 0; }, [] { return 0; });
        assert(make_restore().generation == 9); // the command keeps the captured generation

        auto original = make_marker();
        present_model model(7);
        test_frame first(model, original);
        first.complete(model);
        first.presented(model);

        presented_frame found{};
        assert(model.poll(found));
        assert(r.allows(found, 8));
        assert(!r.allows(found, 9)); // content changed after presentation, before poll
        assert(!r.allows(original, 9));

        auto resized = make_marker(102);
        resized.content_revision = 9;
        assert(r.allows(resized, 9));
        assert(!r.allows(resized, 8));

        present_model current(7);
        test_frame next(current, resized);
        next.complete(current);
        next.presented(current);
        assert(current.poll(found) && r.allows(found, 9));
        assert(!r.allows(found, 10)); // cannot certify content that is not published yet

        auto newer_ticket = make_restore();
        newer_ticket.content_revision = 10;
        r.begin(newer_ticket);
        r.pump([] { return 0; }, [] { return 0; });
        assert(!r.allows(resized, 9) && !r.allows(found, 9)); // restore floor is above the frame
        r.fail(-204);
        assert(!r.allows(resized, 9));

        auto retry = make_restore();
        retry.after_frame = 104;
        retry.content_revision = 11;
        r.begin(retry);
        r.pump([] { return 0; }, [] { return 0; });
        auto fresh = make_marker(105);
        fresh.content_revision = 11;
        assert(r.allows(fresh, 11));
        assert(!r.allows(resized, 9));
    }

    std::cout << "Present recovery: busy retirement, fresh scope, stale callbacks, exact restore and failed frame cases passed\n";
}
