#pragma once
#include <cstdint>
#include <cstddef>

#pragma pack(push, 8)
struct mac_target_probe_request {
    uint32_t size, version;
    uint64_t session, source_frame, native_render_buffer;
    uint32_t width, height, phase, reserved;
};

// Every identity here is an observation, never ownership or a presentation record.
struct mac_target_probe_observation {
    mac_target_probe_request request;
    uint64_t serial, stage_thread, stage_ns, render_thread, render_ns, dispatch_token;
    uint64_t metal_device, command_before, queue_before, command_after, queue_after;
    uint64_t pass_before, pass_after, texture, texture_device, pass_texture_before, pass_texture_after;
    uint64_t observer_sequence_before, observer_sequence_after, checked_ns;
    uint32_t width, height, pixel_format, sample_count, texture_type, storage_mode, usage, framebuffer_only;
    uint32_t command_status_before, command_status_after, load_before, store_before, load_after, store_after;
    uint32_t pass_width_before, pass_height_before, pass_format_before, pass_width_after, pass_height_after, pass_format_after;
    uint32_t flags, reason, reserved0, reserved1;
};

struct mac_target_probe_diagnostic {
    uint32_t size, version;
    uint64_t session, main_thread, render_thread, staged, consumed, rejected;
    uint32_t flags, reserved;
    mac_target_probe_observation pre_gui, end_of_frame;
};
#pragma pack(pop)

static_assert(sizeof(mac_target_probe_request) == 48);
static_assert(sizeof(mac_target_probe_observation) == 304);
static_assert(sizeof(mac_target_probe_diagnostic) == 672);
static_assert(offsetof(mac_target_probe_observation, serial) == 48);
static_assert(offsetof(mac_target_probe_observation, width) == 208);
static_assert(offsetof(mac_target_probe_observation, flags) == 288);
static_assert(offsetof(mac_target_probe_diagnostic, pre_gui) == 64);
static_assert(offsetof(mac_target_probe_diagnostic, end_of_frame) == 368);

// Observation flags: 1 Metal device, 2 CB before, 4 CB after, 8 texture,
// 16 texture device matches, 32 texture extent matches request, 64 CB identity
// stable, 128 queue identity stable, 256 drawable acquisition advanced during getters.
// Reason: 0 complete, 1 nil texture, 2 absent device/CB, 3 device mismatch,
// 4 extent mismatch, 5 Objective-C exception.
// Diagnostic flags: 1 latched; 2 source/worker/activation suppressed.

#ifdef __OBJC__
struct IUnityGraphicsMetal;
namespace mac {
    // Called inside the session gate on the Unity render callback only.
    void observe_target(IUnityGraphicsMetal*, mac_target_probe_observation&);
}
#endif
