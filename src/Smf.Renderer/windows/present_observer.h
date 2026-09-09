#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(SMF_PRESENT_OBSERVER_BUILD)
#define SMF_PO_API extern "C" __declspec(dllexport)
#else
#define SMF_PO_API extern "C"
#endif

// Present hook ABI, x64 Windows. Caller-sized, versioned POD only.

enum present_verdict : uint32_t {
    verdict_no_marker = 0,
    verdict_submission_only = 1,
    verdict_test_present = 2,
    verdict_description_failed = 3,
    verdict_resource_failed = 4,
    verdict_resource_mismatch = 5,
    verdict_device_mismatch = 6,
    verdict_size_mismatch = 7,
    verdict_thread_mismatch = 8,
    verdict_swap_chain_changed = 9,
    verdict_present_not_s_ok = 10,
    verdict_context_changed = 11,
    verdict_contended = 12,
    verdict_non_monotonic = 13,
    verdict_unity_submission_only = 14
};

struct present_context {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t epoch;
    uint64_t restore_serial;
    uint64_t after_source_frame;
    uint64_t content_generation;
    uint64_t resource_generation;
};

struct present_marker {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t epoch;
    uint64_t restore_serial;
    uint64_t source_frame;
    uint64_t content_generation;
    uint64_t resource_generation;
};

struct present_event {
    uint32_t size;
    uint32_t version;
    uint64_t sequence;
    uint64_t session;
    uint64_t epoch;
    uint64_t restore_serial;
    uint64_t source_frame;
    uint64_t content_generation;
    uint64_t resource_generation;
    uint64_t marker_serial;
    int64_t marker_qpc;
    int64_t entry_qpc;
    int64_t return_qpc;
    uint64_t swap_chain_identity;
    uint64_t buffer_identity;
    uint64_t device_identity;
    uint64_t hwnd;
    uint32_t api;
    uint32_t thread_id;
    uint32_t marker_thread_id;
    uint32_t sync_interval;
    uint32_t flags;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    int32_t result;
    int32_t validation_result;
    uint32_t verdict;
    uint32_t nested_calls;
    uint64_t marker_buffer_identity;
    uint64_t marker_device_identity;
    uint32_t descriptor_width;
    uint32_t descriptor_height;
    uint32_t swap_effect;
    uint32_t sample_count;
};

// Diagnostic extension of present_event; the v1 event stays as it is.
struct present_event_v2 {
    present_event event;
    uint32_t marker_width;
    uint32_t marker_height;
    uint32_t marker_format;
    uint32_t marker_sample_count;
    uint32_t marker_sample_quality;
    uint32_t actual_sample_quality;
};

struct present_status {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    uint32_t worker_thread;
    int32_t failure_stage;
    int32_t failure_code;
    uint64_t hwnd;
    uint64_t present_entry;
    uint64_t present1_entry;
    uint64_t dxgi_module;
    int64_t qpc_frequency;
    uint64_t session;
    uint64_t epoch;
    uint64_t restore_serial;
    uint64_t last_begun_frame;
    uint64_t hooks_entered;
    uint64_t nested_calls;
    uint64_t foreign_window_calls;
    uint64_t marker_accepted;
    uint64_t marker_rejected;
    uint64_t marker_superseded;
    uint64_t events_produced;
    uint64_t events_dropped;
    uint64_t ring_overwrites;
    uint64_t submissions;
    uint64_t lock_misses;
    uint64_t last_event_sequence;
    uint32_t discovery_window_destroyed;
    uint32_t discovery_class_removed;
    uint32_t installed_code_matches;
    uint32_t descriptor_failures;
    uint8_t present_before[16];
    uint8_t present1_before[16];
    uint8_t present_installed[16];
    uint8_t present1_installed[16];
};

struct present_chain_status {
    uint32_t size;
    uint32_t version;
    uint64_t present_relay;
    uint64_t present_terminal;
    uint64_t present_terminal_module;
    uint64_t present1_relay;
    uint64_t present1_terminal;
    uint64_t present1_terminal_module;
    uint32_t present_jumps;
    uint32_t present1_jumps;
    uint32_t present_chained;
    uint32_t present1_chained;
};

static_assert(sizeof(present_context) == 56, "context 56");
static_assert(sizeof(present_marker) == 56, "marker 56");
static_assert(sizeof(present_event) == 208, "event 208");
static_assert(sizeof(present_event_v2) == 232, "event_v2 232");
static_assert(sizeof(present_status) == 272, "status 272");
static_assert(offsetof(present_event, marker_qpc) == 72, "event.marker_qpc");
static_assert(offsetof(present_event, verdict) == 168, "event.verdict");
static_assert(sizeof(present_chain_status) == 72, "chain_status 72");

// Returns 1 once the install worker has been started for this process. Status state:
// 0 never requested, 1 installing, 2 hooks installed, 3 failed. Never call from DllMain.
SMF_PO_API int smf_po_start(uint64_t original_hwnd);
// session 0 clears observation but leaves the hooks in place. Epochs must increase.
// Returns 0 on contention; never waits for the render or Present thread.
SMF_PO_API int smf_po_context(const present_context* context);
// Native render callbacks only. Begin must run on every source frame so a skipped
// frame expires its marker; rendered() must follow on the same frame and thread.
SMF_PO_API int smf_po_begin_source_frame(uint64_t session, uint64_t epoch, uint64_t source_frame);
// actual_backbuffer is the live ID3D11Texture2D behind the native render target.
SMF_PO_API int smf_po_rendered(const present_marker* marker, void* actual_backbuffer);
// Unity end-of-frame mode: the camera target may be copied into the DXGI buffer later
// in the same frame, so the next Present is matched by device, size and format instead
// of resource identity (verdict 14).
SMF_PO_API int smf_po_rendered_unity_target(const present_marker* marker, void* completed_unity_target);
SMF_PO_API int smf_po_status(present_status* status);
SMF_PO_API int smf_po_chain_status(present_chain_status* status);
// Bounded, nonblocking (at most 64 events). Returns the count, or -1 on contention.
SMF_PO_API int smf_po_read(uint64_t after_sequence, present_event* events, uint32_t capacity);
SMF_PO_API int smf_po_read_v2(uint64_t after_sequence, present_event_v2* events, uint32_t capacity);
// Latest successful submission matching the whole context. Submission, not display.
SMF_PO_API int smf_po_last_submission(const present_context* context, present_event* event);
