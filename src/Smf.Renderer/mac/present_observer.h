#pragma once
#include "present_status.h"
#include "source_rejection.h"
#include "native_target.h"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "unity/IUnityGraphicsMetal.h"

// Hooks the original CAMetalLayer, Unity's command queue and its buffers to
// follow the staged source texture through to the presented drawable.
namespace present_observer {
    int install(uint64_t session, CAMetalLayer* layer); // AppKit main, before the first capture
    int source(IUnityGraphicsMetal*, const mac_source_target&, const session_native_frame&); // render callback
    bool poll(presented_frame&);
    bool available();
    int failure(uint64_t session); // latched fault, never a presentation
    void stop(uint64_t session);
    int remove(uint64_t session); // main; busy while GPU or callback leases remain
    int snapshot(uint64_t session, present_status&);
    int first_source_rejection(uint64_t session, source_rejection&);
    bool discard_owned_unsubmitted_copy(id<MTLCommandBuffer>); // the owner certifies no commit attempt
}
