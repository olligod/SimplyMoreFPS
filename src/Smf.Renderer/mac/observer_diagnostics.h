#pragma once
#include <cstdint>
#include <cstddef>

// Diagnostic records for the drawable observer. Separate from the session packets.
#pragma pack(push, 8)
struct mac_selection_diagnostic {
    uint64_t call_thread, sequence, latest_sequence, record_thread, drawable, drawable_layer;
    uint64_t texture, texture_device, acquired_ns, checked_ns;
    uint32_t requested_width, requested_height, width, height, pixel_format, flags, reason, operation;
};

struct mac_observer_diagnostic {
    uint32_t size, version;
    uint64_t session, render_thread;
    uint64_t next_calls, observed, marked, presented, dropped, overflow, latest_sequence, in_flight;
    uint64_t original_layer, original_device, acquire_calls, arm_calls, selection_mutex_misses;
    uint32_t flags, reserved;
    mac_selection_diagnostic acquire, arm;
};
#pragma pack(pop)

static_assert(sizeof(mac_selection_diagnostic) == 112, "ObserverSelection112");
static_assert(sizeof(mac_observer_diagnostic) == 360, "ObserverDiagnostic360");
static_assert(offsetof(mac_observer_diagnostic, acquire) == 136, "ObserverAcquire136");
static_assert(offsetof(mac_observer_diagnostic, arm) == 248, "ObserverArm248");

// Selection flags: record=1, done=2, marked=4, drawable=8, matching layer=16,
// matching device=32, framebufferOnly=64, texture=128, metadata cached at nextDrawable=256.
// Diagnostic flags: installed=1, stopping=2, geometryFlipped at install=8,
// original framebufferOnly=16, current record=32.
// Reason: 0 accepted; 2 stopping; 3 no record; 4 newer acquisition; 5 thread;
// 6 already presented; 7 weak drawable expired; 8 layer; 9 no texture;
// 10 extent; 11 device; 12 framebufferOnly; 13 already marked.
// Operation: 2 arm. Zero is never attempted.
