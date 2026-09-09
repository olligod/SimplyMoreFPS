#include "../session_transport.h"
#include "../session_handoff.h"
#include "../capture_diagnostic.h"
#include "../failure_diagnostic.h"
#include "../camera_tuple.h"
#include "../slot_storage.h"
#include "../geometry_policy.h"
#include "../swap_trace.h"
#include "../../common/wheel_modifiers.h"
#include <array>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

using namespace linux_session;

static void storage_policy_tests() {
    slot_storage storage;
    storage.generation = 4;
    storage.content = 3;
    storage.width = 1280;
    storage.height = 720;
    storage.textures[base_storage] = {10, 1280, 720};
    storage.textures[world_storage] = {11, 1280, 720};
    storage.textures[hud_storage] = {12, 1280, 720};
    storage.textures[cache_storage] = {13, 2048, 2048};
    cache_packet cache{900, 5000, 2048, 2048, 1, 0, {2048. / 108, 0, 2048. * 4 / 108, 0, -2048. / 108, 2048. * 104 / 108}};
    storage.copied_cache = cache;
    storage.cache_valid = true;
    lease_policy lease;

    assert(storage.has_names());
    assert(storage.reusable(lease, 4, 3, 1280, 720));
    assert(storage.cache_hit(true, 4, 3, cache));

    assert(lease.begin());
    assert(!storage.reusable(lease, 4, 3, 1280, 720));
    assert(lease.publish(true));
    assert(!storage.reusable(lease, 4, 3, 1280, 720));
    assert(!lease.acquire(false));
    assert(!storage.reusable(lease, 4, 3, 1280, 720));
    assert(lease.acquire(true));
    assert(!storage.reusable(lease, 4, 3, 1280, 720));
    assert(lease.hand_back(true));
    assert(!lease.reclaim(false));
    assert(!storage.reusable(lease, 4, 3, 1280, 720));
    assert(!storage_needs_purge(storage, lease, true, true, true)); // off cannot delete a pending consumer
    assert(lease.reclaim(true));
    assert(storage.reusable(lease, 4, 3, 1280, 720));

    assert(!storage_needs_purge(storage, lease, true, false, false));
    assert(storage_needs_purge(storage, lease, true, true, false));
    assert(storage_needs_purge(storage, lease, false, false, false));
    assert(storage_needs_purge(storage, lease, true, false, true));

    assert(!storage.reusable(lease, 5, 3, 1280, 720));
    assert(!storage.reusable(lease, 4, 4, 1280, 720));
    assert(!storage.reusable(lease, 4, 3, 1024, 768));
    assert(!storage.cache_hit(false, 4, 3, cache));
    assert(!storage.cache_hit(true, 5, 3, cache));
    assert(!storage.cache_hit(true, 4, 4, cache));

    for (unsigned change = 0; change < 7; change++) {
        auto changed = cache;
        switch (change) {
        case 0:
            changed.texture++;
            break;
        case 1:
            changed.serial++;
            break;
        case 2:
            changed.width++;
            break;
        case 3:
            changed.height++;
            break;
        case 4:
            changed.flags = 3;
            break;
        case 5:
            changed.affine[2] += .01;
            break;
        default:
            changed.affine[4] *= 1.01;
            break;
        }
        assert(!storage.cache_hit(true, 4, 3, changed));
    }

    storage.cache_valid = false;
    assert(!storage.cache_hit(true, 4, 3, cache)); // any incomplete copy or fence drops cache validity
    assert(storage.has_names());

    assert(logical_layer_mask(false, true) == 5); // a retained allocation never draws an absent world or cache
    assert(logical_layer_mask(true, true) == 15);
    assert(logical_layer_mask(true, false) == 7);

    lease_policy abandoned;
    assert(abandoned.begin());
    assert(abandoned.publish(true));
    assert(!abandoned.retire_unacquired(false));
    assert(!storage.reusable(abandoned, 4, 3, 1280, 720));
    assert(abandoned.retire_unacquired(true));
    assert(storage.reusable(abandoned, 4, 3, 1280, 720));

    lease_policy quarantine;
    assert(quarantine.begin());
    assert(!quarantine.publish(false));
    assert(!storage.reusable(quarantine, 4, 3, 1280, 720));
    assert(!storage_needs_purge(storage, quarantine, true, true, true));

    std::array<lease_policy, 6> held{};
    for (auto& s : held) {
        assert(s.begin());
        assert(s.publish(true));
        assert(s.acquire(true));
        assert(!storage.reusable(s, 4, 3, 1280, 720));
    }

    assert(held[2].hand_back(true));
    assert(held[2].reclaim(true));
    assert(storage.reusable(held[2], 4, 3, 1280, 720));

    // Free storage still blocks release until the source owner deletes it.
    assert(storage.has_names());
    storage = {};
    assert(!storage.has_names());
}

int main() {
    using namespace camera_control;

    assert(wheel_modifiers_allowed(8, wheel_modifiers(true, true, true)));
    const uint32_t observed_modifiers = wheel_modifiers(true, false, false);
    assert(!wheel_modifiers_allowed(8 | wheel_control, observed_modifiers));
    assert(wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(false, false, false)));
    assert(!wheel_modifiers_allowed(8 | wheel_control, observed_modifiers)); // a release cannot change the queued snapshot
    assert(wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(false, true, true)));
    assert(!wheel_modifiers_allowed(8 | wheel_alt, wheel_modifiers(false, true, false)));
    assert(!wheel_modifiers_allowed(8 | wheel_shift, wheel_modifiers(false, false, true)));

    storage_policy_tests();

    assert(classify_swap_route(false, true, false, false) == route_other_target);
    assert(classify_swap_route(true, false, true, true) == route_original);
    assert(classify_swap_route(true, true, true, true) == route_hidden_source_owner);
    assert(classify_swap_route(true, true, false, true) == route_hidden_same_context_other_thread);
    assert(classify_swap_route(true, true, false, false) == route_hidden_other_context);

    swap_trace trace;
    swap_trace_packet trace_read;
    assert(!trace.read(trace_read, 100));
    trace.reset(2);
    trace.configure(123);
    swap_sample route_sample;
    route_sample.words[0] = trace.enter(route_hidden_same_context_other_thread);
    route_sample.words[2] = 200;
    route_sample.words[3] = 38;
    route_sample.words[4] = 83;
    route_sample.words[13] = 10485766;
    trace.finish(route_hidden_same_context_other_thread, route_sample);

    assert(trace.read(trace_read, 300));
    assert(trace_read.session == 2);
    assert(trace_read.entered[3] == 1);
    assert(trace_read.returned[3] == 1);
    assert(trace_read.last[3].words[13] == 10485766);
    assert(trace.read(trace_read, 400));
    assert(trace_read.last[3].words[3] == 38);
    assert(trace_read.last[3].words[4] == 83);

    trace.reset(3);
    assert(trace.read(trace_read, 500));
    assert(trace_read.entered[3] == 0);
    assert(trace_read.last[3].words[0] == 0);

    assert(classify_geometry(true, 0, 2, 1280, 720) == geometry_outcome::ready);
    assert(classify_geometry(true, 0, 2, 1024, 768, {1280, 720}) == geometry_outcome::stale);
    // The next iteration may freeze the old 1280x720 bundle into the new 1024x768
    // drawable only once both queries agree on that ticket.
    assert(classify_geometry(true, 0, 2, 1024, 768) == geometry_outcome::ready);
    assert(classify_geometry(true, 0, 2, 1024, 768, {1024, 768}) == geometry_outcome::ready);
    assert(classify_geometry(true, 0, 0, 1280, 720) == geometry_outcome::stale);
    assert(classify_geometry(true, 0, 1, 1280, 720) == geometry_outcome::stale);
    assert(classify_geometry(true, 0, 2, 0, 720) == geometry_outcome::stale);
    assert(classify_geometry(true, 0, 2, 1280, 0) == geometry_outcome::stale);
    assert(classify_geometry(false, 0, 2, 1280, 720) == geometry_outcome::failed);
    assert(classify_geometry(true, 1, 0, 0, 0) == geometry_outcome::failed);
    // Activation waits for its exact prepared extent through resize and minimization.
    assert(classify_geometry(true, 0, 2, 1280, 720, {1024, 768}) == geometry_outcome::stale);
    assert(classify_geometry(true, 0, 0, 1024, 768, {1024, 768}) == geometry_outcome::stale);

    assert(!failed_activation_submission(draw_outcome::stale_geometry, false, false));
    assert(failed_activation_submission(draw_outcome::drawn, false, false)); // a failed swap-and-arm
    assert(failed_activation_submission(draw_outcome::failed, false, false));
    assert(!failed_activation_submission(draw_outcome::drawn, false, true));
    assert(!failed_activation_submission(draw_outcome::drawn, true, false));
    // Observed c15: image epoch 4 / map 0 against desired epoch 5 / map 0 before the scene fence.
    assert(!matching_camera_tuple(4, 0, 5, 0));
    assert(matching_camera_tuple(4, 0, 4, 0));
    assert(matching_camera_tuple(5, 0, 5, 0));
    assert(!matching_camera_tuple(5, 0, 5, 1));
    assert(!matching_camera_tuple(5, 1, 4, 0));
    assert(stale_camera_tuple(true, 4, 0, 5, 0));
    assert(!stale_camera_tuple(true, 5, 0, 5, 0));
    // Activation draws its pinned source tuple with camera = false, so even an
    // unrelated prepared camera identity cannot make that draw stale.
    assert(!stale_camera_tuple(false, 4, 0, 5, 1));

    failure_diagnostic fault;
    failure_diagnostic observation;
    assert(!latch_failure(fault, observation));

    observation.line = 261;
    observation.session = 2;
    observation.error = -203;
    observation.thread = 359;
    observation.draw[1] = 2;
    observation.draw[5] = 41;
    observation.draw[14] = 42;
    assert(latch_failure(fault, observation));

    observation.line = 300;
    observation.draw[14] = 99;
    assert(!latch_failure(fault, observation));
    assert(fault.line == 261);
    assert(fault.draw[5] == 41);
    assert(fault.draw[14] == 42);
    assert(fault.error == -203);

    snapshot<failure_diagnostic> failure_box;
    failure_box.publish_boundary(fault, 700);
    failure_diagnostic duplicate;
    uint64_t fault_sequence = 0;
    int64_t fault_time = 0;
    assert(failure_box.read(duplicate, fault_sequence, fault_time));
    assert(duplicate.draw[14] == 42);
    assert(failure_box.read(duplicate, fault_sequence, fault_time));
    assert(duplicate.line == 261);
    assert(fault_time == 700);

    capture_diagnostic first;
    capture_diagnostic observed;
    observed.stage = capture_cache_copy;
    observed.session = 2;
    observed.frame.source_frame = 5722;
    observed.frame.generation = 4;
    observed.frame.content_revision = 3;
    observed.copy.words[1] = 4;

    assert(remember_first_failure(first, observed));
    observed.stage = capture_absent_world;
    observed.frame.source_frame = 5737;
    assert(!remember_first_failure(first, observed));

    assert(first.stage == capture_cache_copy);
    assert(first.frame.source_frame == 5722);
    assert(first.frame.generation == 4);
    assert(first.frame.content_revision == 3);
    assert(first.copy.words[1] == 4);

    command_inbox inbox;
    command_packet command{};
    command.serial = 10;
    command.operation = 3;
    assert(inbox.try_accept(command) == 0);
    command_packet consumed{};
    assert(inbox.try_consume(consumed));
    assert(consumed.serial == 10);

    command.serial = 11;
    command.operation = 6;
    assert(inbox.try_accept(command) == 0);
    assert(inbox.activation.cancelled());
    assert(!inbox.activation.commit(10));
    assert(inbox.try_consume(consumed));

    command.serial = 12;
    command.operation = 3;
    assert(inbox.try_accept(command) == 0);
    assert(inbox.activation.commit(12));
    assert(!inbox.activation.commit(12));
    assert(inbox.try_consume(consumed));
    assert(inbox.try_accept(command) < 0);

    assert(inbox.reset());
    command.serial = 1;
    assert(inbox.try_accept(command) == 0);
    assert(!inbox.reset());
    assert(inbox.try_consume(consumed));
    assert(inbox.reset());

    surface_handoff state;
    state.reset(99, 1);
    assert(state.worker_staged());
    frame_key key{1, 2, 3, 100, 0};
    presented frame{key, 99, true, true, true, true, false};
    assert(state.warmup_ready(frame, key));

    frame.server_processed = false;
    assert(!state.warmup_ready(frame, key));
    frame.server_processed = true;
    assert(!state.worker_released_hidden(true, false, true));
    assert(state.worker_released_hidden(true, true, true));
    assert(!state.worker_bound_original(true));
    assert(state.source_bound_hidden(true, true));
    assert(state.worker_bound_original(true));
    assert(state.activation_ready(frame, key));

    assert(state.begin_restore(13));
    assert(!state.activation_ready(frame, key));
    assert(!state.worker_released_original(false, true, true, true));
    assert(state.worker_released_original(true, true, true, true));
    assert(state.source_bound_original(105, true, true));
    assert(state.routing_acknowledged(13));

    frame.key.restore = 13;
    frame.key.generation = 0;
    frame.key.frame = 105;
    assert(!state.native_available(frame, 104, 3));
    frame.key.frame = 106;
    assert(!state.native_available(frame, 106, 3));
    frame.x_error = true;
    assert(!state.native_available(frame, 105, 3));
    frame.x_error = false;
    frame.key.generation = 2;
    assert(!state.native_available(frame, 105, 3));
    frame.key.generation = 0;
    frame.key.session = 2;
    assert(!state.native_available(frame, 105, 3));
    frame.key.session = 1;
    frame.key.restore = 12;
    assert(!state.native_available(frame, 105, 3));
    frame.key.restore = 13;
    frame.key.content = 0;
    assert(!state.native_available(frame, 105, 3));
    frame.key.content = 2;
    assert(!state.native_available(frame, 105, 3));
    frame.key.content = 4;
    assert(state.native_available(frame, 105, 3));

    assert(!state.retired(true, true, false));
    assert(state.retired(true, true, true));

    surface_handoff failed;
    failed.reset(99, 2);
    assert(failed.begin_restore(20));
    assert(!failed.retired_without_worker(false, true, true));
    assert(!failed.retired_without_worker(true, false, true));
    assert(!failed.retired_without_worker(true, true, false));
    assert(failed.retired_without_worker(true, true, true));

    // Preparation was accepted but failed before a worker started. After source
    // retirement no render callback consumes Stop; main may, once every GPU,
    // dispatch and hook owner is gone.
    command_inbox absent;
    command = {};
    command.serial = 1;
    command.operation = 0;
    assert(absent.try_accept(command) == 0);
    assert(absent.try_consume(consumed));

    command.serial = 2;
    command.operation = 6;
    assert(absent.try_accept(command) == 0);
    assert(absent.try_consume(consumed));

    command.serial = 3;
    command.operation = 11;
    assert(absent.try_accept(command) == 0);

    assert(!can_pump_stopped(false, false, false, false, false));
    assert(!can_pump_stopped(true, true, false, false, false));
    assert(!can_pump_stopped(true, false, true, false, false));
    assert(!can_pump_stopped(true, false, false, true, false));
    assert(!can_pump_stopped(true, false, false, false, true));
    assert(can_pump_stopped(true, false, false, false, false));

    assert(absent.try_consume(consumed));
    assert(consumed.operation == 11);
    assert(absent.empty());

    snapshot<status_packet> box;
    status_packet status{};
    uint64_t sequence = 0;
    int64_t published_at = 0;
    assert(!box.read(status, sequence, published_at));

    status.session = 1;
    status.native_reveal_frame = 106;
    assert(box.try_publish(status, 500));
    assert(box.read(status, sequence, published_at));
    assert(sequence == 1);
    assert(published_at == 500);
    assert(status.native_reveal_frame == 106);
    assert(box.read(status, sequence, published_at));
    assert(sequence == 1);
    assert(published_at == 500);

    status.session = 2;
    box.publish_boundary(status, 600);
    assert(box.read(status, sequence, published_at));
    assert(sequence == 2);
    assert(published_at == 600);
    assert(status.session == 2);

    snapshot<status_packet> concurrent;
    std::atomic<bool> finished{false};
    std::atomic<int> errors{0};

    std::thread writer([&] {
        for (uint64_t i = 1; i <= 10000; i++) {
            status_packet packet{};
            packet.session = i;
            packet.native_reveal_frame = i * 3;
            concurrent.try_publish(packet, int64_t(i));
        }
        finished = true;
    });

    do {
        status_packet packet{};
        uint64_t seq = 0;
        int64_t stamp = 0;
        if (concurrent.read(packet, seq, stamp) && (packet.native_reveal_frame != packet.session * 3 || stamp != int64_t(packet.session))) errors++;
    } while (!finished.load());

    writer.join();
    assert(errors == 0);
    std::cout << "PASS: hidden session routing floors, exact presentation facts, cancel/activation CAS, independent POD transport\n";
}
