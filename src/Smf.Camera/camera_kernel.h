#pragma once
#include <stdint.h>
#include <stddef.h>

// C ABI of the NativeAOT camera kernel (Smf.Camera). Rules for callers:
// - Every packet starts with version (SMF_CAMERA_VERSION) and size; both are checked on every call.
// - create, adopt, configure, step and release must all run on the same worker thread.
// - After create, pass output->session to every later call. release leaves a header-only pose.
// - Never unload this module while the game process is alive.

#if defined(_WIN32)
#define SMF_CAMERA_CALL __cdecl
#else
#define SMF_CAMERA_CALL
#endif

#if defined(__cplusplus)
#define SMF_CAMERA_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#define SMF_CAMERA_ASSERT(c, m) _Static_assert(c, m)
#endif

enum smf_camera_result {
    SMF_CAMERA_OK = 0,
    SMF_CAMERA_STALE = 1,
    SMF_CAMERA_INVALID_ARGUMENT = -1,
    SMF_CAMERA_WRONG_THREAD = -2,
    SMF_CAMERA_BUSY = -3,
    SMF_CAMERA_BAD_SESSION = -4,
    SMF_CAMERA_WRONG_EPOCH = -5,
    SMF_CAMERA_FAULTED = -6,
    SMF_CAMERA_INTERNAL_ERROR = -7
};

enum smf_camera_settings_flags {
    SMF_CAMERA_SMOOTH_ZOOM = 1,
    SMF_CAMERA_ZOOM_TO_MOUSE = 2,
    SMF_CAMERA_EDGE_SCROLL = 4,
    SMF_CAMERA_DISABLE_ZOOM_TO_MOUSE_WHILE_SHIFT_HELD = 8
};

enum smf_camera_input_flags {
    SMF_CAMERA_ZOOM_IN_PULSE = 1,
    SMF_CAMERA_ZOOM_OUT_PULSE = 2,
    SMF_CAMERA_FAST_PAN = 4,
    SMF_CAMERA_MOTION_BLOCKED = 8,
    SMF_CAMERA_MIDDLE_RELEASED_PULSE = 16,
    SMF_CAMERA_ALLOW_EDGE_SCROLL = 32,
    SMF_CAMERA_FULLSCREEN = 64,
    SMF_CAMERA_POINTER_OVER_UI = 128,
    SMF_CAMERA_CLOCK_ONLY = 256
};

enum smf_camera_curve_mode {
    SMF_CAMERA_CURVE_CONSTANT = 0,
    SMF_CAMERA_CURVE_POWER_RANGE = 1,
    SMF_CAMERA_CURVE_POLYNOMIAL2 = 2,
    SMF_CAMERA_CURVE_STEP = 3,
    SMF_CAMERA_CURVE_PIECEWISE_LINEAR = 4
};

enum smf_camera_curve_domain {
    SMF_CAMERA_CURRENT_LOGICAL_ZOOM = 0,
    SMF_CAMERA_DESIRED_LOGICAL_ZOOM = 1,
    SMF_CAMERA_PROJECTION_HALF_HEIGHT = 2
};

enum {
    SMF_CAMERA_VERSION = 2,
    SMF_CAMERA_CURVE_CLAMP_INPUT = 1,
    SMF_CAMERA_PAN_COMPLETED = 1
};

#pragma pack(push, 8)

// Absent curves and unused point slots must be all zero.
typedef struct smf_camera_curve {
    uint32_t mode;
    uint32_t domain;
    uint32_t point_count;
    uint32_t flags;
    double input_min;
    double input_max;
    double a;
    double b;
    double c;
    double exponent;
    double x[16];
    double y[16];
} smf_camera_curve;

// present_mask bits 0..4 select projection, keyboard_rate, edge_rate, move_speed, zoom_speed.
typedef struct smf_camera_profile {
    uint32_t present_mask;
    uint32_t reserved;
    smf_camera_curve projection;
    smf_camera_curve keyboard_rate;
    smf_camera_curve edge_rate;
    smf_camera_curve move_speed;
    smf_camera_curve zoom_speed;
} smf_camera_profile;

// kind 1 is a quintic pan. id 0 (with everything else zero) cancels the active pan.
typedef struct smf_camera_trajectory {
    uint64_t id;
    uint32_t kind;
    uint32_t flags;
    double start_seconds;
    double duration_seconds;
    double source_x;
    double source_z;
    double source_root_size;
    double target_x;
    double target_z;
    double target_root_size;
} smf_camera_trajectory;

typedef struct smf_camera_init {
    uint32_t version;
    uint32_t size;
    uint64_t epoch;
    int32_t map_id;
    uint32_t reserved;
    double x;
    double z;
    double root_size;
    double monotonic_seconds;
} smf_camera_init;

// Bounds use the logical root size, not the projected size. A disabled axis must be all zero.
typedef struct smf_camera_axis_bounds {
    uint32_t enabled;
    uint32_t reserved;
    double minimum_a;
    double minimum_b;
    double minimum_limit_a;
    double minimum_limit_b;
    double maximum_a;
    double maximum_b;
    double maximum_limit_a;
    double maximum_limit_b;
    double collapse_position;
} smf_camera_axis_bounds;

// maximum_size 0 means no cap.
typedef struct smf_camera_bounds {
    smf_camera_axis_bounds x;
    smf_camera_axis_bounds z;
    double maximum_size;
} smf_camera_bounds;

typedef struct smf_camera_settings {
    uint32_t version;
    uint32_t size;
    uint64_t revision;
    uint32_t flags;
    uint32_t reserved;
    double map_width;
    double map_height;
    double pixel_width;
    double pixel_height;
    double ui_scale;
    double min_size;
    double max_size;
    double dolly_rate_keys;
    double dolly_rate_screen_edge;
    double speed_decay;
    double move_speed;
    double zoom_speed;
    double scroll_wheel_rate;
    double zoom_preserve_factor;
    double drag_sensitivity;
    smf_camera_profile profile;
    smf_camera_bounds bounds;
} smf_camera_settings;

// CLOCK_ONLY advances the clock and sequence only; every field after monotonic_seconds
// must then be zero and only MOTION_BLOCKED may accompany it.
typedef struct smf_camera_input {
    uint32_t version;
    uint32_t size;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t settings_revision;
    uint32_t flags;
    uint32_t reserved;
    double monotonic_seconds;
    double pan_x;
    double pan_z;
    double drag_x;
    double drag_y;
    double wheel_delta;
    double pointer_x;
    double pointer_y;
    double inspect_pane_height;
    smf_camera_trajectory trajectory;
} smf_camera_input;

typedef struct smf_camera_pose {
    uint32_t version;
    uint32_t size;
    uint64_t session;
    uint64_t epoch;
    uint64_t pose_sequence;
    uint64_t input_sequence;
    uint64_t settings_revision;
    int32_t map_id;
    int32_t result;
    double x;
    double z;
    double root_size;
    double desired_size;
    double velocity_x;
    double velocity_z;
    double projection_half_height;
    uint64_t active_pan_id;
    uint64_t finished_pan_id;
    uint32_t pan_flags;
    uint32_t reserved;
} smf_camera_pose;

#pragma pack(pop)

int32_t SMF_CAMERA_CALL smf_camera_create(const smf_camera_init* init, uint32_t init_bytes,
    const smf_camera_settings* settings, uint32_t settings_bytes,
    smf_camera_pose* output, uint32_t output_bytes);
int32_t SMF_CAMERA_CALL smf_camera_adopt(uint64_t session, const smf_camera_init* init, uint32_t init_bytes,
    const smf_camera_settings* settings, uint32_t settings_bytes,
    smf_camera_pose* output, uint32_t output_bytes);
int32_t SMF_CAMERA_CALL smf_camera_configure(uint64_t session, uint64_t epoch,
    const smf_camera_settings* settings, uint32_t settings_bytes,
    smf_camera_pose* output, uint32_t output_bytes);
int32_t SMF_CAMERA_CALL smf_camera_step(uint64_t session, const smf_camera_input* input, uint32_t input_bytes,
    smf_camera_pose* output, uint32_t output_bytes);
int32_t SMF_CAMERA_CALL smf_camera_release(uint64_t session, smf_camera_pose* output, uint32_t output_bytes);

#if defined(__cplusplus)
}
#endif

// Layout is a contract with the C# side and between platforms.
#define SMF_CAMERA_OFFSET(t, f, n) SMF_CAMERA_ASSERT(offsetof(t, f) == n, #t "." #f)

SMF_CAMERA_ASSERT(sizeof(smf_camera_init) == 56, "init 56");
SMF_CAMERA_OFFSET(smf_camera_init, version, 0);
SMF_CAMERA_OFFSET(smf_camera_init, size, 4);
SMF_CAMERA_OFFSET(smf_camera_init, epoch, 8);
SMF_CAMERA_OFFSET(smf_camera_init, map_id, 16);
SMF_CAMERA_OFFSET(smf_camera_init, reserved, 20);
SMF_CAMERA_OFFSET(smf_camera_init, x, 24);
SMF_CAMERA_OFFSET(smf_camera_init, z, 32);
SMF_CAMERA_OFFSET(smf_camera_init, root_size, 40);
SMF_CAMERA_OFFSET(smf_camera_init, monotonic_seconds, 48);

SMF_CAMERA_ASSERT(sizeof(smf_camera_settings) == 1920, "settings 1920");
SMF_CAMERA_OFFSET(smf_camera_settings, version, 0);
SMF_CAMERA_OFFSET(smf_camera_settings, size, 4);
SMF_CAMERA_OFFSET(smf_camera_settings, revision, 8);
SMF_CAMERA_OFFSET(smf_camera_settings, flags, 16);
SMF_CAMERA_OFFSET(smf_camera_settings, reserved, 20);
SMF_CAMERA_OFFSET(smf_camera_settings, map_width, 24);
SMF_CAMERA_OFFSET(smf_camera_settings, map_height, 32);
SMF_CAMERA_OFFSET(smf_camera_settings, pixel_width, 40);
SMF_CAMERA_OFFSET(smf_camera_settings, pixel_height, 48);
SMF_CAMERA_OFFSET(smf_camera_settings, ui_scale, 56);
SMF_CAMERA_OFFSET(smf_camera_settings, min_size, 64);
SMF_CAMERA_OFFSET(smf_camera_settings, max_size, 72);
SMF_CAMERA_OFFSET(smf_camera_settings, dolly_rate_keys, 80);
SMF_CAMERA_OFFSET(smf_camera_settings, dolly_rate_screen_edge, 88);
SMF_CAMERA_OFFSET(smf_camera_settings, speed_decay, 96);
SMF_CAMERA_OFFSET(smf_camera_settings, move_speed, 104);
SMF_CAMERA_OFFSET(smf_camera_settings, zoom_speed, 112);
SMF_CAMERA_OFFSET(smf_camera_settings, scroll_wheel_rate, 120);
SMF_CAMERA_OFFSET(smf_camera_settings, zoom_preserve_factor, 128);
SMF_CAMERA_OFFSET(smf_camera_settings, drag_sensitivity, 136);
SMF_CAMERA_OFFSET(smf_camera_settings, profile, 144);
SMF_CAMERA_ASSERT(offsetof(smf_camera_settings, bounds) == 1752, "settings.bounds");

SMF_CAMERA_ASSERT(sizeof(smf_camera_input) == 192, "input 192");
SMF_CAMERA_OFFSET(smf_camera_input, version, 0);
SMF_CAMERA_OFFSET(smf_camera_input, size, 4);
SMF_CAMERA_OFFSET(smf_camera_input, epoch, 8);
SMF_CAMERA_OFFSET(smf_camera_input, sequence, 16);
SMF_CAMERA_OFFSET(smf_camera_input, settings_revision, 24);
SMF_CAMERA_OFFSET(smf_camera_input, flags, 32);
SMF_CAMERA_OFFSET(smf_camera_input, reserved, 36);
SMF_CAMERA_OFFSET(smf_camera_input, monotonic_seconds, 40);
SMF_CAMERA_OFFSET(smf_camera_input, pan_x, 48);
SMF_CAMERA_OFFSET(smf_camera_input, pan_z, 56);
SMF_CAMERA_OFFSET(smf_camera_input, drag_x, 64);
SMF_CAMERA_OFFSET(smf_camera_input, drag_y, 72);
SMF_CAMERA_OFFSET(smf_camera_input, wheel_delta, 80);
SMF_CAMERA_OFFSET(smf_camera_input, pointer_x, 88);
SMF_CAMERA_OFFSET(smf_camera_input, pointer_y, 96);
SMF_CAMERA_OFFSET(smf_camera_input, inspect_pane_height, 104);
SMF_CAMERA_OFFSET(smf_camera_input, trajectory, 112);

SMF_CAMERA_ASSERT(sizeof(smf_camera_pose) == 136, "pose 136");
SMF_CAMERA_OFFSET(smf_camera_pose, version, 0);
SMF_CAMERA_OFFSET(smf_camera_pose, size, 4);
SMF_CAMERA_OFFSET(smf_camera_pose, session, 8);
SMF_CAMERA_OFFSET(smf_camera_pose, epoch, 16);
SMF_CAMERA_OFFSET(smf_camera_pose, pose_sequence, 24);
SMF_CAMERA_OFFSET(smf_camera_pose, input_sequence, 32);
SMF_CAMERA_OFFSET(smf_camera_pose, settings_revision, 40);
SMF_CAMERA_OFFSET(smf_camera_pose, map_id, 48);
SMF_CAMERA_OFFSET(smf_camera_pose, result, 52);
SMF_CAMERA_OFFSET(smf_camera_pose, x, 56);
SMF_CAMERA_OFFSET(smf_camera_pose, z, 64);
SMF_CAMERA_OFFSET(smf_camera_pose, root_size, 72);
SMF_CAMERA_OFFSET(smf_camera_pose, desired_size, 80);
SMF_CAMERA_OFFSET(smf_camera_pose, velocity_x, 88);
SMF_CAMERA_OFFSET(smf_camera_pose, velocity_z, 96);
SMF_CAMERA_OFFSET(smf_camera_pose, projection_half_height, 104);
SMF_CAMERA_OFFSET(smf_camera_pose, active_pan_id, 112);
SMF_CAMERA_OFFSET(smf_camera_pose, finished_pan_id, 120);
SMF_CAMERA_OFFSET(smf_camera_pose, pan_flags, 128);
SMF_CAMERA_OFFSET(smf_camera_pose, reserved, 132);

SMF_CAMERA_ASSERT(sizeof(smf_camera_curve) == 320, "curve 320");
SMF_CAMERA_OFFSET(smf_camera_curve, input_min, 16);
SMF_CAMERA_OFFSET(smf_camera_curve, x, 64);
SMF_CAMERA_OFFSET(smf_camera_curve, y, 192);

SMF_CAMERA_ASSERT(sizeof(smf_camera_profile) == 1608, "profile 1608");
SMF_CAMERA_OFFSET(smf_camera_profile, projection, 8);
SMF_CAMERA_OFFSET(smf_camera_profile, zoom_speed, 1288);

SMF_CAMERA_ASSERT(sizeof(smf_camera_trajectory) == 80, "trajectory 80");

SMF_CAMERA_ASSERT(sizeof(smf_camera_axis_bounds) == 80, "axis_bounds 80");
SMF_CAMERA_ASSERT(offsetof(smf_camera_axis_bounds, collapse_position) == 72, "axis_bounds.collapse_position");
SMF_CAMERA_ASSERT(sizeof(smf_camera_bounds) == 168, "bounds 168");
SMF_CAMERA_ASSERT(offsetof(smf_camera_bounds, z) == 80, "bounds.z");
SMF_CAMERA_ASSERT(offsetof(smf_camera_bounds, maximum_size) == 160, "bounds.maximum_size");

#undef SMF_CAMERA_OFFSET
#undef SMF_CAMERA_ASSERT
