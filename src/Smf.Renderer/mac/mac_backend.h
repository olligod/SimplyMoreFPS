#pragma once
#include "session_packets.h"
#include "../common/camera_packets.h"
#include "observer_diagnostics.h"
#include "target_probe.h"
#include "copy_probe.h"
#include "source_target.h"

#define SMF_MAC_API extern "C" __attribute__((visibility("default")))

// Mac-only packets. The caller owns the plain data during the call and the
// source RT until the matching pre-GUI dispatch is accepted.
#pragma pack(push, 8)
struct mac_source_base {
    uint32_t size, version;
    uint64_t session, content_revision, generation, source_frame;
    uint64_t texture, native_render_buffer, original_window, original_layer;
    uint32_t width, height, pixel_format, flags; // flags bit0: top row lives at native row 0
    uint64_t reserved;
};

struct mac_capabilities {
    uint32_t size, version, flags, result;
    uint64_t session, original_window, original_layer, overlay_layer;
    uint64_t native_loads, interfaces, source_device, main_thread, render_thread, worker_thread;
    uint64_t source_completed, worker_completed, worker_presented;
    uint64_t original_presented, original_presented_with_overlay;
};

struct mac_presentation {
    uint32_t size, version;
    uint64_t session, generation, count;
    int64_t timestamp, frequency;
};
#pragma pack(pop)

static_assert(sizeof(mac_source_base) == 96, "MacBase96");
static_assert(sizeof(mac_capabilities) == 136, "MacCapabilities136");
static_assert(sizeof(mac_presentation) == 48, "MacPresentation48");

// mac_capabilities::flags bits.
enum mac_capability : uint32_t {
    capability_graphics_interface = 1,
    capability_source_base_ready = 2,
    capability_original_present_observer = 4,
    capability_original_presented_behind_overlay = 8,
    capability_mouse_observation = 16,
    capability_worker_ready = 32
};

SMF_MAC_API int32_t smf_mac_presentation(mac_presentation*, uint32_t);
// Unity/AppKit main thread only. The source base must name an owned Unity RT;
// this call issues no render event and never waits for one.
SMF_MAC_API int32_t smf_mac_source_base(const mac_source_base*, uint32_t);
// Kept for ABI identification only; always returns unavailable.
SMF_MAC_API int32_t smf_mac_original_base_enable(uint64_t session);
// Main only, before the first Prepare.
SMF_MAC_API int32_t smf_mac_source_target_enable(uint64_t session);
SMF_MAC_API int32_t smf_mac_source_target(const mac_source_target*, uint32_t);
SMF_MAC_API int32_t smf_mac_native_target(const mac_source_target*, uint32_t);
SMF_MAC_API int32_t smf_mac_find_original_window(uint64_t* original_window);
SMF_MAC_API int32_t smf_mac_capabilities(mac_capabilities*, uint32_t);
SMF_MAC_API int32_t smf_mac_observer_diagnostic(mac_observer_diagnostic*, uint32_t);
SMF_MAC_API int64_t smf_session_clock_now();
SMF_MAC_API int64_t smf_session_clock_frequency();
SMF_MAC_API int32_t smf_session_start(uint64_t original_window, uint64_t session);
SMF_MAC_API int32_t smf_session_command(const session_command*, uint32_t);
SMF_MAC_API int32_t smf_session_content_fence(uint64_t, uint64_t);
SMF_MAC_API int32_t smf_session_ack(uint64_t, uint64_t, session_ack*, uint32_t);
SMF_MAC_API int32_t smf_session_status(session_status*, uint32_t);
SMF_MAC_API int32_t smf_session_pre_gui(const session_pre_gui*, uint32_t, void**, int32_t*);
SMF_MAC_API int32_t smf_session_frame(const session_frame*, uint32_t, void**, int32_t*);
SMF_MAC_API int32_t smf_session_native_frame(const session_native_frame*, uint32_t, void**, int32_t*);
SMF_MAC_API int32_t smf_session_cancel(void*, int32_t);
SMF_MAC_API void* smf_session_render_event();
SMF_MAC_API int32_t smf_session_poll_joined(uint64_t);
SMF_MAC_API int32_t smf_session_quit();
// The kernel path is a UTF-8 absolute path to the camera dylib, separate from
// the Unity-loaded renderer bundle. Only the compositor worker loads and calls it.
SMF_MAC_API double smf_camera_bridge_now();
SMF_MAC_API int32_t smf_camera_bridge_init(const char*, uint32_t);
SMF_MAC_API int32_t smf_camera_bridge_publish(const smf_bridge_main*, uint32_t);
SMF_MAC_API int32_t smf_camera_bridge_revoke(uint64_t, uint32_t);
SMF_MAC_API int32_t smf_camera_bridge_desired(smf_bridge_desired*, uint32_t);
SMF_MAC_API int32_t smf_camera_bridge_status(smf_bridge_status*, uint32_t);
SMF_MAC_API int32_t smf_camera_control_policy(const smf_control_policy*, uint32_t);
SMF_MAC_API int32_t smf_camera_control_impulse(const smf_control_impulse*, uint32_t);
SMF_MAC_API int32_t smf_camera_control_status(smf_control_status*, uint32_t);
SMF_MAC_API int32_t smf_camera_control_wheel_status(smf_control_wheel_status*, uint32_t);

// Diagnostic-only probe modes, enabled before the first Prepare. They never
// change the original target or create a worker.
SMF_MAC_API int32_t smf_mac_target_probe_enable(uint64_t session);
SMF_MAC_API int32_t smf_mac_target_probe_stage(const mac_target_probe_request*, uint32_t);
SMF_MAC_API int32_t smf_mac_target_probe_diagnostic(mac_target_probe_diagnostic*, uint32_t);
// Requires target-probe mode. Copies bind to the existing target ticket.
SMF_MAC_API int32_t smf_mac_copy_probe_enable(uint64_t session);
SMF_MAC_API int32_t smf_mac_copy_probe_stage(const mac_copy_probe_request*, uint32_t);
SMF_MAC_API int32_t smf_mac_copy_probe_record(uint64_t session, uint64_t capture_id, mac_copy_probe_record*, uint32_t);
SMF_MAC_API int32_t smf_mac_copy_probe_readback(uint64_t session, uint64_t capture_id, uint32_t route, void*, uint32_t);
