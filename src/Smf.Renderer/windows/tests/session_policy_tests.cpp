// Pure lifecycle decisions from session_policy.h and present_format.h. No graphics.
#include "../session_policy.h"
#include "../present_format.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace session;

static int checks = 0;

static void check(bool value, const char* name) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", name);
        std::exit(1);
    }
}

int main() {
    check(decide_cancel(7, 8, false, 7) == cancel_decision::mark, "unissued live ticket is marked for render release");
    check(decide_cancel(7, 8, true, 0) == cancel_decision::complete, "retry after render consumption is complete");
    check(decide_cancel(7, 9, false, 8) == cancel_decision::complete, "retry must not cancel a slot's successor");
    check(decide_cancel(0, 8, false, 7) == cancel_decision::invalid, "zero token rejected");
    check(decide_cancel(-1, 8, false, 7) == cancel_decision::invalid, "negative token rejected");
    check(decide_cancel(8, 8, false, 7) == cancel_decision::invalid, "not-yet-issued token rejected");

    check(visual_bundle_complete(true, true, true), "attachment needs background, map and HUD");
    check(!visual_bundle_complete(false, true, true), "missing background is not attached");
    check(!visual_bundle_complete(true, false, true), "missing map is not attached");
    check(!visual_bundle_complete(true, true, false), "missing HUD is not attached");
    check(!visual_bundle_complete(false, false, false), "removed triple is detached");

    session_cache sample{};
    sample.height = 2048;
    sample.flags = 1;

    check(cache_background_source_y(sample) == 0, "unflipped top-left samples the first row");
    sample.flags = 3;
    check(cache_background_source_y(sample) == 2047, "flipped top-left samples the last row");

    check(copy_region_fits(2048, 2048, 0, cache_background_source_y(sample), 1, 1), "flipped background texel stays in bounds");
    check(!copy_region_fits(2048, 2048, 0, 2047, 1, 2), "background sample cannot cross the last row");
    check(!copy_region_fits(2048, 2048, 0, 2048, 1, 1), "one-past-source row rejected");
    check(!copy_region_fits(UINT32_MAX, 2048, UINT32_MAX, 0, 1, 1), "copy source span cannot wrap");
    check(!copy_region_fits(2048, 2048, 0, 0, 0, 1), "empty source rectangle rejected");

    check(frame_layout_matches(424, 424, 4), "frame version 4 at 424 bytes accepted");
    check(!frame_layout_matches(336, 336, 1), "obsolete 336 byte frame rejected");
    check(!frame_layout_matches(368, 368, 2), "obsolete 368 byte frame rejected");
    check(!frame_layout_matches(416, 416, 2), "old version cannot reinterpret the cache tail");
    check(!frame_layout_matches(416, 368, 3), "caller size mismatch rejected");
    check(!frame_layout_matches(416, 416, 3), "frame without scene description rejected");
    check(!frame_layout_matches(424, 424, 3), "old version cannot reinterpret scene description");

    session_frame frame{};
    check(cache_valid(frame), "no-map cache descriptor must be all zero");
    frame.flags = session_has_map;
    check(!cache_valid(frame), "map frame needs a completed full-map cache");

    frame.source_frame = 200;
    frame.world_texture = 11;
    frame.hud_texture = 12;
    frame.cache = {13, 100, 2048, 2048, 1, 0, {8, 0, 0, 0, -8, 2048}};
    check(cache_valid(frame), "older completed cache usable by a newer source pose");

    const auto original = frame.cache;
    check(decide_cache_update({}, original) == cache_update::copy, "first cache copies once");
    check(decide_cache_update(original, original) == cache_update::keep, "same serial keeps the copied pixels");
    frame.cache.serial = 101;
    frame.cache.affine[2] = 32;
    frame.cache.texture = 14;
    check(decide_cache_update(original, frame.cache) == cache_update::copy, "new serial allows a refreshed texture and affine");

    frame.cache = original;
    frame.cache.serial = 99;
    check(decide_cache_update(original, frame.cache) == cache_update::reject, "late serial cannot replace newer pixels");
    frame.cache = original;
    frame.cache.affine[2] = 1;
    check(decide_cache_update(original, frame.cache) == cache_update::reject, "same serial cannot change the affine");
    frame.cache = original;
    frame.cache.texture = 14;
    check(decide_cache_update(original, frame.cache) == cache_update::reject, "same serial cannot change the texture");

    frame.cache = original;
    frame.cache.serial = 101;
    frame.cache.width = 4096;
    check(decide_cache_update(original, frame.cache) == cache_update::reject, "resized cache needs a new generation");
    frame.cache = original;
    frame.cache.serial = 101;
    frame.cache.flags = 3;
    check(decide_cache_update(original, frame.cache) == cache_update::reject, "changed row order needs a new generation");
    check(decide_cache_update(original, {}) == cache_update::reject, "ready cache cannot disappear within a generation");

    frame.cache = original;
    frame.cache.serial = 201;
    check(!cache_valid(frame), "future capture serial rejected");
    frame.cache = original;
    frame.cache.serial = 0;
    check(!cache_valid(frame), "ready cache needs a serial");

    frame.cache = original;
    frame.cache.texture = 11;
    check(!cache_valid(frame), "world texture cannot pose as the cache");
    frame.cache = original;
    frame.cache.texture = 12;
    check(!cache_valid(frame), "HUD texture cannot pose as the cache");

    frame.cache = original;
    frame.cache.reserved = 1;
    check(!cache_valid(frame), "reserved field rejected");
    frame.cache = original;
    frame.cache.flags = 2;
    check(!cache_valid(frame), "flip without ready rejected");
    frame.cache = original;
    frame.cache.flags = 3;
    check(cache_valid(frame), "flipped cache texture accepted");
    frame.cache = original;
    frame.cache.flags = 5;
    check(!cache_valid(frame), "unknown cache flags rejected");

    frame.cache = original;
    frame.cache.affine[4] = 0;
    check(!cache_valid(frame), "singular cache affine rejected");
    frame.cache = original;
    frame.cache.affine[2] = NAN;
    check(!cache_valid(frame), "non-finite cache origin rejected");

    frame.cache = original;
    frame.cache.width = 16385;
    check(!cache_valid(frame), "D3D dimension bound enforced");
    frame.cache = original;
    frame.cache.width = 16384;
    frame.cache.height = 16384;
    check(!cache_valid(frame), "cache memory bound enforced");

    check(present_format::matches(28, 27, true), "Unity typeless RGBA target matches the typed buffer");
    check(present_format::matches(87, 91, true), "BGRA family matches across typed views");
    check(!present_format::matches(28, 27, false), "strict buffer mode keeps the exact format");
    check(!present_format::matches(28, 87, true), "RGBA and BGRA never match");
    check(!present_format::matches(28, 24, true), "different channel layout never matches");

    check(decide_start(0, 1, false, false, false) == start_decision::create, "first session is created");
    check(decide_start(1, 1, false, true, true) == start_decision::stale, "joined session identity cannot be reused");
    check(decide_start(1, 2, false, true, true) == start_decision::create, "joined session one can restart as two");
    check(decide_start(1, 2, true, true, true) == start_decision::busy, "new identity waits for the worker to join");
    check(decide_start(1, 1, true, false, true) == start_decision::already_accepted, "same live request is idempotent");
    check(decide_start(1, 1, true, false, false) == start_decision::stale, "idempotency needs the original window");
    check(decide_start(2, 1, false, true, true) == start_decision::stale, "older session identity rejected");

    session_status s{};
    s.active_generation = 31;
    s.generations[0].generation = 31;
    s.generations[0].content_revision = 4;
    s.generations[0].frames = 12;
    s.generations[0].state = 3;
    auto& old = s.generations[0];

    check(accept_content(8, 4, false, old, 8, 4, 31), "complete old bundle accepts the same content");
    check(!retain_historical(s, 4), "healthy bundle is not historical");
    check(retain_historical(s, 5), "new content fence retains the complete old bundle");
    check(!accept_content(8, 5, false, old, 8, 4, 31), "late old-content frame rejected");
    check(!accept_content(8, 5, false, old, 8, 5, 31), "new content cannot borrow the old generation");
    check(!accept_content(9, 4, false, old, 8, 4, 31), "late old session rejected");
    check(!accept_content(8, 4, true, old, 8, 4, 31), "restore seals captures before native routing");

    session_command activation{};
    activation.operation = op_activate;
    activation.content_revision = 8;
    check(!content_superseded(activation, 0, 8), "current activation may begin");
    check(content_superseded(activation, 0, 9), "obsolete activation is rejected before visibility changes");
    check(!content_superseded(activation, 1, 9), "committed activation survives a later content fence");
    check(!content_superseded(activation, 2, 9), "activation completion reports the generation that became live");
    activation.operation = op_prepare_replacement;
    check(content_superseded(activation, 1, 9), "obsolete hidden preparation still supersedes");
    activation.operation = op_restore_native;
    check(!content_superseded(activation, 0, 9), "content cannot supersede native restoration");

    auto& next = s.generations[1];
    next.generation = 32;
    next.content_revision = 5;
    next.state = 1;
    check(accept_content(8, 5, false, next, 8, 5, 32), "hidden successor may capture");
    check(retain_historical(s, 5), "hidden successor does not replace the active history");

    next.frames = 1;
    next.state = 3;
    s.active_generation = 32;
    old.state = 2;
    check(!retain_historical(s, 5), "complete successor ends the historical retention");

    old.state = 4;
    check(!accept_content(8, 4, false, old, 8, 4, 31), "retiring generation rejects queued work");
    old.state = 5;
    check(!accept_content(8, 4, false, old, 8, 4, 31), "retired slot cannot revive");

    s.flags = 2;
    s.native_submitted_restore_serial = 17;
    s.native_submitted_frame = 101;
    s.native_present_serial = 9;
    s.native_backbuffer = 0x123;

    check(native_handoff_ready(s, 17, 100), "later native Present completes the handoff");
    check(!native_handoff_ready(s, 16, 100), "old restore serial never completes it");
    check(!native_handoff_ready(s, 17, 101), "same frame never completes it");
    check(!native_handoff_ready(s, 0, 100), "warmup submission never completes it");

    s.native_present_serial = 0;
    check(!native_handoff_ready(s, 17, 100), "end of frame alone is not a handoff");
    s.native_present_serial = 9;
    s.native_backbuffer = 0;
    check(!native_handoff_ready(s, 17, 100), "missing native backbuffer fails the handoff");

    s.native_backbuffer = 0x123;
    s.flags = 0;
    check(!native_handoff_ready(s, 17, 100), "unsubmitted Present never permits detach");

    check(ticket_holds_generation(1, 31, 31), "queued pre-GUI ticket blocks generation retirement");
    check(ticket_holds_generation(2, 31, 31), "queued frame ticket blocks generation retirement");
    check(!ticket_holds_generation(2, 32, 31), "successor traffic does not block predecessor retirement");
    check(!ticket_holds_generation(3, 31, 31), "native marker owns no generation texture");

    std::printf("PASS %d CPU lifecycle policy checks; no graphics or game execution\n", checks);
    return 0;
}
