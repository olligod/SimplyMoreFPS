#include "../source_target.h"
#include "../packet_policy.h"
#include "../../common/wheel_modifiers.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

int main() {
    {
        using namespace camera_control;
        assert(wheel_modifiers_allowed(8, wheel_modifiers(true, true, true)));
        const uint32_t observed = wheel_modifiers(true, false, false);
        assert(!wheel_modifiers_allowed(8 | wheel_control, observed));
        assert(wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(false, false, false)));
        assert(!wheel_modifiers_allowed(8 | wheel_control, observed)); // a release cannot change a queued snapshot
        assert(wheel_modifiers_allowed(8 | wheel_control, wheel_modifiers(false, true, true)));
        assert(!wheel_modifiers_allowed(8 | wheel_alt, wheel_modifiers(false, true, false)));
        assert(!wheel_modifiers_allowed(8 | wheel_shift, wheel_modifiers(false, false, true)));
    }

    using namespace mac;
    mac_source_target target{64, 1, 7, 8, 9, 100, 0x1000, 1280, 720, 0, 0};
    session_pre_gui pre{64, 1, 7, 8, 9, 100, 1280, 720, 0, 1, 0};
    assert(valid_source_target(target) && source_target_matches(target, pre));

    // Every identity component and both dimensions independently break the match.
    for (unsigned field = 0; field < 6; ++field) {
        auto changed = pre;
        if (field == 0) ++changed.session;
        if (field == 1) ++changed.content_revision;
        if (field == 2) ++changed.generation;
        if (field == 3) ++changed.source_frame;
        if (field == 4) ++changed.width;
        if (field == 5) ++changed.height;
        assert(!source_target_matches(target, changed));
    }

    for (unsigned field = 0; field < 11; ++field) {
        auto bad = target;
        if (field == 0) bad.size = 63;
        if (field == 1) bad.version = 2;
        if (field == 2) bad.session = 0;
        if (field == 3) bad.content_revision = 0;
        if (field == 4) bad.generation = 0;
        if (field == 5) bad.source_frame = 0;
        if (field == 6) bad.native_render_buffer = 0;
        if (field == 7) bad.width = 0;
        if (field == 8) bad.height = 0;
        if (field == 9) bad.flags = 1;
        if (field == 10) bad.reserved = 1;
        assert(!valid_source_target(bad));
    }

    auto edge = target;
    edge.width = edge.height = 4096;
    assert(valid_source_target(edge));
    edge.height = 4097;
    assert(!valid_source_target(edge));
    edge.width = 16384;
    edge.height = 1024;
    assert(valid_source_target(edge));
    edge.width = 16385;
    assert(!valid_source_target(edge));
    edge.width = edge.height = std::numeric_limits<uint32_t>::max();
    assert(!valid_source_target(edge));

    for (uint32_t field = 0; field < 4; ++field) {
        auto bad = pre;
        if (field == 0) bad.size = 0;
        if (field == 1) bad.version = 2;
        if (field == 2) bad.reserved = 1;
        if (field == 3) bad.flags = 2;
        assert(!source_target_matches(target, bad));
    }

    auto no_map = pre;
    no_map.flags = 0;
    assert(source_target_matches(target, no_map));

    source_target_stage stage;
    assert(!stage.matches(pre));
    assert(stage.stage(target, 99) == 0 && stage.matches(pre));

    // Checking a match and a busy ticket reservation do not erase the stage.
    auto original = stage.value;
    assert(stage.matches(pre) && stage.matches(pre));
    assert(!std::memcmp(&original, &stage.value, sizeof(original)));
    assert(stage.stage(target, 99) == 0); // exact retry before ticket acceptance

    auto conflict = target;
    conflict.native_render_buffer += 8;
    assert(stage.stage(conflict, 99) == -201 && stage.matches(pre));
    auto older = target;
    --older.source_frame;
    assert(stage.stage(older, 98) == 1 && stage.matches(pre));

    auto fresh = target;
    ++fresh.source_frame;
    assert(stage.stage(fresh, 99) == 0 && !stage.matches(pre));
    auto fresh_pre = pre;
    ++fresh_pre.source_frame;
    assert(stage.matches(fresh_pre));

    stage.clear();
    assert(!stage.matches(fresh_pre));
    // Accepted frames cannot be resubmitted even after their stage was cleared.
    assert(stage.stage(fresh, 101) == 1 && !stage.value.size);
    assert(stage.stage(target, 101) == 1 && !stage.value.size);

    auto replacement = target;
    ++replacement.generation;
    ++replacement.content_revision;
    replacement.source_frame = 102;
    assert(stage.stage(replacement, 0) == 0);
    stage.clear();
    assert(!stage.value.native_render_buffer);

    // Complete and absent worlds mean the same with and without a map or flips.
    for (uint32_t flags = 0; flags < 64; ++flags) {
        for (uint32_t count : {0u, 1u, 2u, 0xffffffffu}) {
            const bool known = flags < 32;
            const bool complete = (flags & 2u) != 0;
            const bool absent = (flags & 4u) != 0;
            const bool expected = known && (complete != absent) && ((complete && count > 0) || (absent && count == 0));
            assert(valid_world_dispatch(flags, count) == expected);
        }
    }

    assert(valid_world_dispatch(5, 0));
    assert(valid_world_dispatch(3, 1));
    assert(!valid_world_dispatch(5, 1) && !valid_world_dispatch(3, 0) && !valid_world_dispatch(7, 0));

    session_native_frame marker{64, 1, 7, 0, 200, 8, 9, 1280, 720, 0, 0};
    assert(native_marker_queue_policy(marker, 7, 12, 190, false) == 0);
    assert(native_marker_queue_policy(marker, 7, 12, 190, true) == 1);

    auto restore = marker;
    restore.restore_serial = 12;
    assert(native_marker_queue_policy(restore, 7, 12, 190, false) == 0);
    assert(native_marker_queue_policy(restore, 7, 12, 190, true) == 1);
    restore.source_frame = 190;
    assert(native_marker_queue_policy(restore, 7, 12, 190, true) == -201);
    restore.flags = 1;
    assert(native_marker_queue_policy(restore, 7, 12, 190, true) == 1); // begin-only expiry
    restore.restore_serial = 11;
    assert(native_marker_queue_policy(restore, 7, 12, 190, true) == -201);

    // Retirement changes only a valid marker's availability, never validation.
    for (unsigned field = 0; field < 10; ++field) {
        auto bad = marker;
        if (field == 0) bad.size = 0;
        if (field == 1) bad.version = 2;
        if (field == 2) bad.session = 8;
        if (field == 3) bad.source_frame = 0;
        if (field == 4) bad.width = 0;
        if (field == 5) bad.height = 0;
        if (field == 6) bad.width = 16385;
        if (field == 7) bad.flags = 2;
        if (field == 8) bad.reserved = 1;
        if (field == 9) bad.restore_serial = 13;
        assert(native_marker_queue_policy(bad, 7, 12, 190, false) == -201);
        assert(native_marker_queue_policy(bad, 7, 12, 190, true) == -201);
    }

    // A pump-time prepare failure must survive capture queue validation. Native
    // restoration markers stay queueable but still pass the identity checks.
    for (int fault : {-11, -201, -203, -204}) {
        assert(capture_queue_owner_result(1, fault) == fault);
        assert(capture_queue_owner_result(2, fault) == fault);
        assert(capture_queue_owner_result(3, fault) == 0);

        auto recovery = marker;
        recovery.restore_serial = 12;
        recovery.generation = 0;
        assert(native_marker_queue_policy(recovery, 7, 12, 190, false) == 0);
        assert(native_marker_queue_policy(recovery, 7, 12, 190, true) == 1);
        recovery.session = 8;
        assert(native_marker_queue_policy(recovery, 7, 12, 190, false) == -201);
        recovery.session = 7;
        recovery.source_frame = 190;
        assert(native_marker_queue_policy(recovery, 7, 12, 190, false) == -201);
    }

    for (uint32_t kind : {1u, 2u, 3u}) {
        for (int result : {0, 1}) {
            assert(capture_queue_owner_result(kind, result) == 0);
        }
    }

    // Complete-native markers without a generation keep their prior meaning.
    marker.content_revision = marker.generation = 0;
    assert(native_marker_queue_policy(marker, 7, 12, 190, false) == 0);
    assert(native_marker_queue_policy(marker, 7, 12, 190, true) == 1);

    std::cout << "PASS: Target64 joins and stage lifetime; world-dispatch parity; valid retired marker availability\n";
}
