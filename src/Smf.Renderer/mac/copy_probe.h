#pragma once
#include "target_probe.h"

#pragma pack(push, 8)
struct mac_copy_probe_request {
    mac_target_probe_request target;
    uint64_t capture_id;
    uint32_t marker_x, marker_y, marker_width, marker_height, pattern, reserved;
};

struct mac_copy_probe_record {
    uint32_t size, version;
    mac_copy_probe_request request;
    mac_target_probe_observation target;
    uint64_t source_texture, owned_texture, direct_buffer, via_buffer, control_source_buffer, control_buffer;
    uint64_t command_buffer, command_queue, completion_command_buffer, completion_queue, completion_thread;
    uint64_t begin_ns, end_unity_ns, encoded_ns, completion_ns, readback_bytes, row_bytes, direct_bytes, via_bytes, control_bytes;
    int64_t native_error_code;
    uint32_t state, reason, command_status_at_encode, command_status_complete, store_before, store_after;
    uint32_t flags, read_mask, reserved0, reserved1, reserved2, reserved3;
};
#pragma pack(pop)

static_assert(sizeof(mac_copy_probe_request) == 80);
static_assert(offsetof(mac_copy_probe_request, capture_id) == 48);
static_assert(offsetof(mac_copy_probe_request, marker_x) == 56);
static_assert(sizeof(mac_copy_probe_record) == 608);
static_assert(offsetof(mac_copy_probe_record, request) == 8);
static_assert(offsetof(mac_copy_probe_record, target) == 88);
static_assert(offsetof(mac_copy_probe_record, source_texture) == 392);
static_assert(offsetof(mac_copy_probe_record, native_error_code) == 552);
static_assert(offsetof(mac_copy_probe_record, state) == 560);
static_assert(offsetof(mac_copy_probe_record, flags) == 584);

// capture_id 1..4: four fixed slots, never reused within a process. pattern 1 or 2
// names the Unity-drawn marker epoch; native code never draws marker pixels.
// state: 0 absent, 1 staged, 2 queued, 3 encoding, 4 encoded, 5 completed,
// 6 rejected before GPU commands, 7 quarantined after possible GPU commands.
// reason: 0 none, 1 target metadata, 2 target/attachment mismatch, 3 allocation,
// 4 CB/queue changed, 5 encoder unavailable, 6 native exception, 7 completion
// identity/status/error, 8 dispatch mismatch, 9 cancellation.
// flags: 1 matched attachment; 2 ended Unity encoder; 4 own encoder created;
// 8 own encoder ended; 16 completion registered; 32 completion observed.
// read_mask: bit0 direct, bit1 via owned texture, bit2 GPU buffer control.

#ifdef __OBJC__
namespace mac {
    // Main thread only unless noted. 0 success, 1 busy or not ready, negative rejected.
    int enable_copy_probe(uint64_t session);
    int stage_copy_probe(const mac_copy_probe_request&);
    int bind_copy_probe(const mac_target_probe_request&);
    void cancel_copy_probe(int index);
    void retire_copy_probe_stages();
    void execute_copy_probe(int index, IUnityGraphicsMetal*, const mac_target_probe_observation&); // render callback
    int read_copy_probe(uint64_t session, uint64_t capture_id, mac_copy_probe_record&);
    int read_copy_bytes(uint64_t session, uint64_t capture_id, uint32_t route, void*, uint32_t bytes);
    bool copy_probe_in_flight();
}
#endif
