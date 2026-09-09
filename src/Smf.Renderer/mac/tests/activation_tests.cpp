#include "../activation.h"
#include "../present_recovery.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>

using namespace mac;

std::shared_ptr<activation_attempt> make_attempt(uint64_t operation = 71, uint64_t shown = 100) {
    return std::make_shared<activation_attempt>(7, operation, 9, 8, shown);
}

void bind_candidate(activation_candidate& candidate, const std::shared_ptr<activation_attempt>& attempt, uint64_t frame = 101,
    uint64_t generation = 9, uint64_t content = 8, uint64_t acquired = 110) {
    candidate.bind(attempt, frame, generation, content, acquired);
}

int main() {
    // Show pending: initial enable submits no onscreen drawable; a replacement
    // retains its already displayed generation. Only an exact visible attempt
    // opens the candidate submission path, without needing a prior own draw.
    for (uint64_t active : {uint64_t(0), uint64_t(6)}) {
        for (int n = 0; n < 100; ++n) {
            assert(activation_draw_generation(active, 9, true, false) == active);
        }

        auto attempt = make_attempt();
        assert(activation_draw_generation(active, 9, false, true) == active);
        assert(activation_draw_generation(active, 9, true, attempt->matches(7, 71, 9, 8, 99)) == active);
        assert(activation_draw_generation(active, 9, true, attempt->matches(7, 71, 9, 8, 100)) == 9);

        activation_candidate pre_show;
        bind_candidate(pre_show, attempt, 100, 9, 8, 99);
        pre_show.gpu_completed(true);
        pre_show.presented(true);
        assert(!attempt->frame());

        activation_candidate first_eligible;
        bind_candidate(first_eligible, attempt);
        first_eligible.presented(true);
        assert(!attempt->frame());
        first_eligible.gpu_completed(true);
        assert(attempt->frame() == 101);
    }

    // Deterministic gate interleaving: worker processes GPU completion and
    // reserves the next draw under one gate; main sees only its final state.
    // Detach must leave an idle state observable, not immediately replace it.
    {
        bool in_flight = true;
        bool gpu_done = false;
        bool visible = true;
        unsigned draws = 1;

        auto worker = [&](bool detach) {
            if (in_flight && gpu_done) {
                in_flight = false;
                gpu_done = false;
            }
            if (worker_may_draw(!in_flight, true, detach)) {
                in_flight = true;
                ++draws;
            }
        };

        auto main_detach = [&] {
            if (!in_flight) visible = false;
        };

        for (int n = 0; n < 10; ++n) {
            worker(true);
            main_detach();
            assert(in_flight && visible && draws == 1);
        }

        gpu_done = true;
        worker(true);
        main_detach();
        assert(!in_flight && !visible && draws == 1);

        for (int n = 0; n < 10; ++n) {
            worker(true);
            assert(!in_flight && draws == 1);
        }
    }

    // Neither routing a restore nor awaiting its native frame is Detach:
    // keep the retained overlay drawing until the explicit detach ticket.
    for (bool awaiting_native : {false, true}) {
        (void)awaiting_native;
        bool in_flight = true;
        unsigned draws = 1;

        for (int n = 0; n < 100; ++n) {
            in_flight = false; // actual previous GPU completion
            if (worker_may_draw(!in_flight, true, false)) {
                in_flight = true;
                ++draws;
            }
            assert(in_flight);
        }

        assert(draws == 101);
    }

    assert(!worker_may_draw(false, true, false));
    assert(!worker_may_draw(true, false, false));

    // Initial activation cannot borrow an unactivated, merely visible layer.
    assert(!activation_ready(false, false, 0, false));
    assert(!activation_ready(false, true, 0, false));
    assert(activation_ready(true, true, 0, false));

    // A positively activated visible owner permits replacement while original
    // observation is stopped. Losing visibility or restoring removes that path.
    assert(activation_ready(false, true, 9, false));
    assert(!activation_ready(false, false, 9, false));
    assert(!activation_ready(true, true, 9, true));

    {
        using namespace present_observer;
        present_model old(7);
        auto source = old.register_buffer(1, true, 0);
        session_native_frame marker{64, 1, 7, 0, 100, 8, 9, 1280, 720, 0, 0};
        assert(old.eof(source, 50, marker));
        assert(old.begin_queue(source, false, 0));
        old.end_queue(source, true, 2);
        old.stop();
        assert(!old.gpu_idle());

        // Active capture cannot accumulate a write-only history or certify a
        // native restore after observation stops. Actual GPU completion drains.
        for (unsigned n = 0; n < 2000; ++n) {
            assert(!old.register_buffer(n + 2, true, 0));
            assert(!old.write(source, 50));
        }

        old.complete(source, 1, true);
        presented_frame frame{};
        assert(old.gpu_idle() && !old.live_buffers() && !old.poll(frame) && old.fault == fault_kind::none);

        recovery restorer;
        session_command restore{};
        restore.session = 7;
        restore.serial = 71;
        restore.generation = 9;
        restore.content_revision = 8;
        restore.after_frame = 100;
        restorer.begin(restore);
        restorer.pump([] { return 0; }, [] { return 0; });

        present_model fresh(7);
        auto buffer = fresh.register_buffer(1, true, 0);
        marker.restore_serial = 71;
        marker.source_frame = 101;
        marker.generation = 0;
        assert(fresh.eof(buffer, 50, marker));

        auto drawable = fresh.acquire(3, 51, 1280, 720, 80, 1000);
        assert(fresh.full_copy(buffer, 50, 51, drawable, 1280, 720, 80, true));
        assert(fresh.begin_queue(buffer, false, 0));
        fresh.end_queue(buffer, true, 2);
        fresh.complete(buffer, 1, true);

        assert(!fresh.poll(frame));
        fresh.present(drawable, 3, buffer);
        assert(!fresh.poll(frame));
        fresh.presented(drawable, 3, 1, 1100);
        assert(fresh.poll(frame));
        assert(restorer.allows(frame, 8));
    }

    for (bool positive_first : {false, true}) {
        auto attempt = make_attempt();
        activation_candidate candidate;
        bind_candidate(candidate, attempt);

        if (positive_first) {
            candidate.presented(true);
        } else {
            candidate.gpu_completed(true);
        }

        assert(!attempt->frame());
        if (positive_first) {
            candidate.gpu_completed(true);
        } else {
            candidate.presented(true);
        }
        assert(attempt->frame() == 101);
    }

    for (bool missing : {false, true}) {
        auto attempt = make_attempt();
        activation_candidate first;
        activation_candidate later;
        bind_candidate(first, attempt);
        bind_candidate(later, attempt, 102);

        first.gpu_completed(true);
        if (!missing) first.presented(false);
        assert(!attempt->frame());

        later.gpu_completed(true);
        later.presented(true);
        assert(attempt->frame() == 102);
    }

    {
        auto attempt = make_attempt();
        activation_candidate delayed;
        bind_candidate(delayed, attempt);
        delayed.gpu_completed(true);

        for (uint64_t frame = 102; frame < 202; ++frame) {
            activation_candidate newer;
            bind_candidate(newer, attempt, frame);
            newer.gpu_completed(true);
        }

        assert(!attempt->frame());
        delayed.presented(true);
        assert(attempt->frame() == 101);
    }

    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        auto attempt = make_attempt();
        activation_candidate candidate;
        bind_candidate(candidate, attempt, invalid == 0 ? 0 : 101, invalid == 1 ? 10 : 9, invalid == 2 ? 7 : 8, invalid == 3 ? 100 : 110);

        candidate.gpu_completed(invalid != 4);
        candidate.presented(invalid != 5);
        assert(!attempt->frame());
    }

    {
        auto old = make_attempt();
        auto replacement = make_attempt(72, 120);
        activation_candidate late;
        activation_candidate current;
        bind_candidate(late, old);
        bind_candidate(current, replacement, 102, 9, 8, 130);

        old.reset();
        late.gpu_completed(true);
        late.presented(true);

        assert(!replacement->frame());
        assert(!replacement->matches(7, 71, 9, 8, 100));
        assert(!replacement->matches(7, 72, 9, 8, 100));
        assert(replacement->matches(7, 72, 9, 8, 120));

        current.presented(true);
        current.gpu_completed(true);
        assert(replacement->frame() == 102);
    }

    // Concurrent callbacks must join: independent release-store/acquire-load
    // flags could both miss the other publication and strand the receipt.
    for (int i = 0; i < 200; ++i) {
        auto attempt = make_attempt();
        activation_candidate candidate;
        bind_candidate(candidate, attempt);

        std::thread gpu([&] { candidate.gpu_completed(true); });
        candidate.presented(true);
        gpu.join();
        assert(attempt->frame() == 101);
    }

    // The first qualifying attempt stays available despite any later candidates.
    {
        auto attempt = make_attempt();
        activation_candidate first;
        activation_candidate second;
        bind_candidate(first, attempt);
        bind_candidate(second, attempt, 102);

        first.gpu_completed(true);
        first.presented(true);
        second.presented(true);
        second.gpu_completed(true);
        assert(attempt->frame() == 101);
    }

    std::cout << "Activation: detach drain, restore continuity, dropped/missing and delayed receipts, callback join, invalid evidence and stale attempts passed\n";
}
