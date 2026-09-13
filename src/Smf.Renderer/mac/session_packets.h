#pragma once
#include <stdint.h>
#include <stddef.h>

// Session packets shared with the C# side. Sizes and offsets are a contract.
// The qpc fields keep their layout; on macOS they hold mach_absolute_time in ns.
#pragma pack(push, 8)
// The no-map case requires all-zero bytes.
struct session_pose {
    uint32_t size, version;
    uint64_t frame_id, unity_frame, camera_id, epoch;
    float x, y, z, orthographic_size;
    float pixel_x, pixel_y, pixel_width, pixel_height;
    float world_to_camera[16], projection[16];
    uint64_t bridge_epoch, applied_sequence, model_revision;
    int32_t map_id;
    uint32_t reserved;
    double root_x, root_y, root_z, root_size;
};

struct session_command {
    uint32_t size, version;
    uint64_t session, serial, generation, content_revision, previous_generation, after_frame;
    uint32_t operation, flags, width, height;
    float empty_background[4];
};

struct session_pre_gui {
    uint32_t size, version;
    uint64_t session, content_revision, generation, source_frame;
    uint32_t width, height;
    uint64_t bootstrap_texture;
    uint32_t flags, reserved;
};

struct session_cache {
    uint64_t texture, serial;
    uint32_t width, height, flags, reserved;
    // World X/Z to logical top-left cache texels (a, b, c, d, e, f), before the optional row flip.
    double affine[6];
};

struct session_frame {
    uint32_t size, version;
    uint64_t session, content_revision, generation, source_frame;
    uint64_t world_texture, hud_texture;
    int64_t eof_qpc;
    uint32_t flags, world_dispatches;
    session_pose pose;
    session_cache cache;
    uint64_t scene_description;
};

struct session_native_frame {
    uint32_t size, version;
    uint64_t session, restore_serial, source_frame, content_revision, generation;
    uint32_t width, height, flags, reserved; // flags bit0: begin-only marker, expires frames but is not a complete native frame
};

struct session_ack {
    uint32_t size, version;
    uint64_t session, serial, generation, content_revision, source_frame;
    uint32_t operation, evidence;
    int32_t result;
    uint32_t disposition; // 1 success, 2 superseded before activation, 3 failure
    uint64_t commit_serial;
    int64_t completed_qpc;
};

struct session_generation_status {
    uint64_t generation, content_revision, last_source_frame, prepared_frame;
    uint64_t frames, source_commit, completed_commit, attachment, attachment_completed, retired_serial;
    uint32_t state, width, height, flags; // state: 0 empty, 1 preparing, 2 ready, 3 active, 4 retiring, 5 retired, 6 failed
    uint64_t retire_requested, staged_frame;
    uint32_t base_format, world_format, hud_format, reserved;
};

struct session_status {
    uint32_t size, version;
    uint64_t session, content_fence, content_acknowledged, operation_fence, active_generation, active_frame, last_ack_serial;
    uint32_t main_thread, render_thread, worker_thread, completion_thread;
    uint32_t worker_state, stage;
    int32_t result;
    uint32_t flags; // bit0 original window root; bit1 native frame presented; bit4 native frame presented behind the overlay
    uint64_t source_commit, source_completed, worker_commit, worker_completed;
    uint64_t dropped_frames, queued_callbacks;
    session_generation_status generations[2];
    uint64_t native_ordered_frame, native_ordered_restore_serial, native_submitted_frame, native_reveal_frame;
    uint64_t native_submitted_generation, native_submitted_content, native_submitted_restore_serial;
    uint64_t native_present_serial, native_backbuffer;
};
#pragma pack(pop)

static_assert(sizeof(session_pose) == 264 && offsetof(session_pose, root_x) == 232, "Pose264");
static_assert(sizeof(session_command) == 88, "Command88");
static_assert(sizeof(session_pre_gui) == 64, "PreGui64");
static_assert(sizeof(session_cache) == 80 && offsetof(session_cache, affine) == 32, "Cache80");
static_assert(sizeof(session_frame) == 424 && offsetof(session_frame, pose) == 72 &&
    offsetof(session_frame, cache) == 336 && offsetof(session_frame, scene_description) == 416, "Frame424 version4");
static_assert(sizeof(session_native_frame) == 64, "NativeFrame64");
static_assert(sizeof(session_ack) == 80, "Ack80");
static_assert(sizeof(session_generation_status) == 128, "Generation128");
static_assert(sizeof(session_status) == 472, "Status472");
