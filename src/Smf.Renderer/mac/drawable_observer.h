#pragma once
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <cstdint>
#include "observer_diagnostics.h"

// Watches nextDrawable on the original CAMetalLayer and records which native
// frame each presented drawable carried.
namespace drawable_observer {

    struct marker {
        uint64_t session = 0, frame = 0, generation = 0, content = 0, restore = 0;
        uint32_t width = 0, height = 0;
    };

    struct presented_frame : marker {
        uint64_t serial = 0, drawable = 0, acquired_ns = 0, presented_ns = 0;
        double presented_time = 0;
        bool overlay_visible = false;
    };

    struct status {
        uint64_t next_calls = 0, observed = 0, marked = 0, presented = 0, dropped = 0, overflow = 0;
        uint64_t presented_behind_overlay = 0, last_sequence = 0, in_flight = 0;
        bool installed = false, stopping = false;
    };

    // Main only. The layer must belong to the window; only this layer instance's
    // class changes, never a global CAMetalLayer or command buffer swizzle.
    int install(CAMetalLayer* original, NSWindow* window);
    // Main only, after capture routing was cancelled and drained. Returns 1 while
    // dispatched methods are still unwinding.
    int remove();
    void set_overlay_visible(bool); // main; diagnostic only, never an ack
    bool available();
    status snapshot();
    // Copies cached facts only; never touches a drawable, Unity or the GPU.
    int read_diagnostic(mac_observer_diagnostic&);
    // Render callback only, on the thread that obtained the latest drawable.
    bool arm(const marker&); // the ordered EOF marker, not its begin-only form
    bool poll(presented_frame&); // returns only an actual positive presentation
}
