#pragma once
#include "../../Smf.Camera/camera_kernel.h"
#include <stdint.h>
#include <stddef.h>

// Packets exchanged between the renderer's camera bridge and the mod. Camera epochs
// are independent from the renderer's image epochs.

#pragma pack(push, 8)

struct smf_bridge_desired {
    uint32_t version;
    uint32_t size;
    uint64_t epoch;
    uint64_t sequence;
    int32_t map_id;
    uint32_t flags;
    double x;
    double z;
    double root_size;
    double projection_half_height;
    uint64_t active_pan_id;
    uint64_t finished_pan_id;
    uint32_t pan_flags;
    uint32_t reserved;
};

struct smf_bridge_main_state {
    uint32_t version;
    uint32_t size;
    uint64_t epoch;
    uint64_t applied_sequence;
    uint64_t source_frame;
    int32_t map_id;
    uint32_t flags;
    double x;
    double z;
    double root_size;
    double min_size;
    double max_size;
    double ui_scale;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t reason;
    uint32_t reserved;
    double projection_half_height;
};

struct smf_bridge_bindings {
    uint32_t version;
    uint32_t size;
    uint64_t revision;
    int32_t up;
    int32_t up2;
    int32_t down;
    int32_t down2;
    int32_t left;
    int32_t left2;
    int32_t right;
    int32_t right2;
};

struct smf_bridge_main {
    uint32_t size;
    uint32_t version;
    uint64_t publication;
    smf_bridge_main_state state;
    smf_camera_settings settings;
    smf_bridge_bindings bindings;
    smf_camera_trajectory trajectory;
};

enum smf_bridge_state : uint32_t {
    bridge_dormant,
    bridge_waiting,
    bridge_seed,
    bridge_owned,
    bridge_blocked,
    bridge_fault
};

struct smf_bridge_status {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    int32_t result;
    uint64_t fence_epoch;
    uint64_t main_revision;
    uint64_t worker_epoch;
    uint64_t seed_sequence;
    uint64_t applied_sequence;
    uint64_t kernel_session;
    uint64_t steps;
    uint64_t adopts;
    uint64_t configs;
    uint64_t mailbox_drops;
    uint64_t commit_sequence;
    int64_t qpc_frequency;
    int64_t step_qpc;
    int64_t commit_qpc;
    uint32_t main_thread;
    uint32_t worker_thread;
    uint32_t flags;
    uint32_t reserved;
    smf_bridge_desired desired;
};

// Renderer control packets. Their layout is independent of the camera kernel packets.

struct smf_control_rect {
    float left;
    float top;
    float right;
    float bottom;
};

struct smf_control_policy {
    uint32_t size;
    uint32_t version;
    uint64_t epoch;
    uint64_t revision;
    uint64_t source_frame;
    uint32_t flags; // 1 edge eligible, 2 fullscreen, 4 wheel unsafe, 8 camera owned, 16/32/64 wheel modifiers
    uint32_t rect_count;
    double ui_scale;
    double inspect_height;
    uint32_t width;
    uint32_t height;
    smf_control_rect rects[64]; // client pixels, top-left origin
};

struct smf_control_impulse {
    uint32_t size;
    uint32_t version;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t source_frame;
    double wheel_delta;
    uint32_t flags; // 1 native zoom-in key, 2 native zoom-out key
    uint32_t reserved;
};

struct smf_control_status {
    uint32_t size;
    uint32_t version;
    uint32_t queued;
    uint32_t high_water;
    uint64_t accepted;
    uint64_t consumed;
    uint64_t stale;
    uint64_t blocked;
    uint64_t full;
    uint64_t busy;
    uint64_t policy_revision;
};

struct smf_control_wheel_status {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    int32_t error;
    uint64_t source_window;
    uint64_t generation;
    uint64_t observed;
    uint64_t queued;
    uint64_t consumed;
    uint64_t denied;
    uint64_t stale;
    uint64_t overflow;
    uint64_t active_epoch;
    uint32_t thread_id;
    uint32_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(smf_bridge_desired) == 88, "bridge_desired 88");
static_assert(offsetof(smf_bridge_desired, projection_half_height) == 56, "bridge_desired.projection_half_height");
static_assert(offsetof(smf_bridge_desired, active_pan_id) == 64, "bridge_desired.active_pan_id");
static_assert(offsetof(smf_bridge_desired, finished_pan_id) == 72, "bridge_desired.finished_pan_id");
static_assert(offsetof(smf_bridge_desired, pan_flags) == 80, "bridge_desired.pan_flags");
static_assert(sizeof(smf_bridge_main_state) == 112, "bridge_main_state 112");
static_assert(offsetof(smf_bridge_main_state, projection_half_height) == 104, "bridge_main_state.projection_half_height");
static_assert(sizeof(smf_bridge_bindings) == 48, "bridge_bindings 48");
static_assert(sizeof(smf_bridge_main) == 2176, "bridge_main 2176");
static_assert(offsetof(smf_bridge_main, state) == 16, "bridge_main.state");
static_assert(offsetof(smf_bridge_main, settings) == 128, "bridge_main.settings");
static_assert(offsetof(smf_bridge_main, bindings) == 2048, "bridge_main.bindings");
static_assert(offsetof(smf_bridge_main, trajectory) == 2096, "bridge_main.trajectory");
static_assert(sizeof(smf_bridge_status) == 232, "bridge_status 232");
static_assert(offsetof(smf_bridge_status, desired) == 144, "bridge_status.desired");

static_assert(sizeof(smf_control_policy) == 1088, "control_policy 1088");
static_assert(offsetof(smf_control_policy, rects) == 64, "control_policy.rects");
static_assert(sizeof(smf_control_impulse) == 48, "control_impulse 48");
static_assert(sizeof(smf_control_status) == 72, "control_status 72");
static_assert(sizeof(smf_control_wheel_status) == 96, "control_wheel_status 96");
