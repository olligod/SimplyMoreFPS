#pragma once
#include <stdint.h>
#include <stddef.h>

// Windows x64 POD ABI shared with the C# side. No managed references or callbacks
// cross it, and this module stays loaded until process exit. S_OK means accepted or
// completed, S_FALSE means pending or busy, a failure HRESULT never means retired.

enum session_operation : uint32_t {
    op_prepare_hidden = 0,
    op_invalidate_world = 1,
    op_prepare_replacement = 2,
    op_activate = 3,
    op_retire_generation = 4,
    op_release_generation_main = 5,
    op_restore_native = 6,
    op_await_native_frame = 7,
    op_detach = 8,
    op_retire_session = 9,
    op_release_session_main = 10,
    op_stop_worker = 11
};

enum session_evidence : uint32_t {
    evidence_complete_composite = 2,
    evidence_composite_activated = 4,
    evidence_world_invalidated = 8,
    evidence_native_frame_available = 128,
    evidence_composite_detached = 256,
    evidence_render_owners_retired = 512,
    evidence_worker_joined = 2048,
    evidence_never_activated = 4096,
    // Facts only the native side can establish.
    evidence_operation_fence_accepted = 1u << 16,
    evidence_source_render_ordered = 1u << 17,
    evidence_commit_processed = 1u << 18,
    evidence_present_submitted = 1u << 19
};

enum session_flags : uint32_t {
    session_has_map = 1,
    session_world_dispatch_complete = 2,
    session_world_dispatch_absent = 4,
    session_world_flip_y = 8,
    session_hud_flip_y = 16
};

#pragma pack(push, 8)

// Without a map every byte must be zero.
struct session_pose {
    uint32_t size;
    uint32_t version;
    uint64_t frame_id;
    uint64_t unity_frame;
    uint64_t camera_id;
    uint64_t epoch;
    float x;
    float y;
    float z;
    float orthographic_size;
    float pixel_x;
    float pixel_y;
    float pixel_width;
    float pixel_height;
    float world_to_camera[16];
    float projection[16];
    uint64_t camera_epoch;
    uint64_t applied_sequence;
    uint64_t model_revision;
    int32_t map_id;
    uint32_t reserved;
    double root_x;
    double root_y;
    double root_z;
    double root_size;
};

struct session_command {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t serial;
    uint64_t generation;
    uint64_t content_revision;
    uint64_t previous_generation;
    uint64_t after_frame;
    uint32_t operation;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    float empty_background[4];
};

struct session_pre_gui {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t content_revision;
    uint64_t generation;
    uint64_t source_frame;
    uint32_t width;
    uint32_t height;
    uint64_t bootstrap_texture;
    uint32_t flags;
    uint32_t reserved;
};

struct session_cache {
    uint64_t texture;
    uint64_t serial;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t reserved;
    double affine[6]; // world X/Z to top-left cache texels (a, b, c, d, e, f), before any row flip
};

struct session_frame {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t content_revision;
    uint64_t generation;
    uint64_t source_frame;
    uint64_t world_texture;
    uint64_t hud_texture;
    int64_t eof_qpc;
    uint32_t flags;
    uint32_t world_dispatches;
    session_pose pose;
    session_cache cache;
    uint64_t scene_description;
};

struct session_native_frame {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t restore_serial;
    uint64_t source_frame;
    uint64_t content_revision;
    uint64_t generation;
    uint32_t width;
    uint32_t height;
    uint32_t flags; // bit0: frame expiry only, no native marker
    uint32_t reserved;
};

struct session_ack {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t serial;
    uint64_t generation;
    uint64_t content_revision;
    uint64_t source_frame;
    uint32_t operation;
    uint32_t evidence;
    int32_t result;
    uint32_t disposition; // 1 success, 2 superseded before activation, 3 failure
    uint64_t commit_serial;
    int64_t completed_qpc;
};

struct session_generation_status {
    uint64_t generation;
    uint64_t content_revision;
    uint64_t last_source_frame;
    uint64_t prepared_frame;
    uint64_t frames;
    uint64_t source_commit;
    uint64_t completed_commit;
    uint64_t attachment;
    uint64_t attachment_completed;
    uint64_t retired_serial;
    uint32_t state; // 0 empty, 1 preparing, 2 ready, 3 active, 4 retiring, 5 retired, 6 failed
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint64_t retire_requested;
    uint64_t staged_frame;
    uint32_t base_format;
    uint32_t world_format;
    uint32_t hud_format;
    uint32_t reserved;
};

struct session_status {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t content_fence;
    uint64_t content_acknowledged;
    uint64_t operation_fence;
    uint64_t active_generation;
    uint64_t active_frame;
    uint64_t last_ack_serial;
    uint32_t main_thread;
    uint32_t render_thread;
    uint32_t worker_thread;
    uint32_t completion_thread;
    uint32_t worker_state;
    uint32_t stage;
    int32_t result;
    uint32_t flags; // bit0 target attached to the Unity HWND, bit1 native Present submitted, bit2 old bundle retained
    uint64_t source_commit;
    uint64_t source_completed;
    uint64_t worker_commit;
    uint64_t worker_completed;
    uint64_t dropped_frames;
    uint64_t queued_callbacks;
    session_generation_status generations[2];
    uint64_t native_ordered_frame;
    uint64_t native_ordered_restore_serial;
    uint64_t native_submitted_frame;
    uint64_t native_reveal_frame;
    uint64_t native_submitted_generation;
    uint64_t native_submitted_content;
    uint64_t native_submitted_restore_serial;
    uint64_t native_present_serial;
    uint64_t native_backbuffer;
};

#pragma pack(pop)

static_assert(sizeof(session_pose) == 264 && offsetof(session_pose, root_x) == 232, "pose 264");
static_assert(sizeof(session_command) == 88, "command 88");
static_assert(sizeof(session_pre_gui) == 64, "pre_gui 64");
static_assert(sizeof(session_cache) == 80 && offsetof(session_cache, affine) == 32, "cache 80");
static_assert(sizeof(session_frame) == 424 && offsetof(session_frame, pose) == 72 &&
    offsetof(session_frame, cache) == 336 && offsetof(session_frame, scene_description) == 416, "frame 424 version 4");
static_assert(sizeof(session_native_frame) == 64, "native_frame 64");
static_assert(sizeof(session_ack) == 80, "ack 80");
static_assert(sizeof(session_generation_status) == 128, "generation_status 128");
static_assert(sizeof(session_status) == 472, "status 472");

#define SMF_SESSION_API extern "C" __declspec(dllexport)

SMF_SESSION_API int32_t __cdecl smf_session_start(uint64_t original_unity_hwnd, uint64_t session);
// The release operations are main-owned and rejected here. restore_native fences off
// older operations and captures before it returns.
SMF_SESSION_API int32_t __cdecl smf_session_command(const session_command*, uint32_t bytes);
SMF_SESSION_API int32_t __cdecl smf_session_content_fence(uint64_t session, uint64_t revision);
SMF_SESSION_API int32_t __cdecl smf_session_ack(uint64_t session, uint64_t serial, session_ack*, uint32_t bytes);
SMF_SESSION_API int32_t __cdecl smf_session_status(session_status*, uint32_t bytes);

// Queued tickets hold POD plus AddRef'd textures. Issue the ticket once through
// GL.IssuePluginEventAndData; the render thread does every read and release.
SMF_SESSION_API int32_t __cdecl smf_session_pre_gui(const session_pre_gui*, uint32_t bytes, void** ticket, int32_t* token);
SMF_SESSION_API int32_t __cdecl smf_session_frame(const session_frame*, uint32_t bytes, void** ticket, int32_t* token);
SMF_SESSION_API int32_t __cdecl smf_session_native_frame(const session_native_frame*, uint32_t bytes, void** ticket, int32_t* token);
SMF_SESSION_API int32_t __cdecl smf_session_cancel(void* ticket, int32_t token);
SMF_SESSION_API void* __cdecl smf_session_render_event(); // (int, void*); event 0 only pumps retirement

// There is no synchronous stop. Poll this after op_stop_worker until it returns S_OK.
SMF_SESSION_API int32_t __cdecl smf_session_poll_joined(uint64_t session);
