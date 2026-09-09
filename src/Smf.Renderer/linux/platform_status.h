#pragma once
#include <cstdint>

namespace linux_session {

    // Linux-only diagnostics, published next to the common status packet.
    struct platform_status {
        uint32_t size = 528;
        uint32_t version = 1;
        uint64_t publication = 0;
        uint64_t published_ns = 0;
        uint64_t original = 0;
        uint64_t source_drawable = 0;
        uint64_t hidden_drawable = 0;
        uint64_t source_context = 0;
        uint64_t worker_context = 0;
        uint32_t phase = 0;
        uint32_t source_thread = 0;
        uint32_t worker_thread = 0;
        uint32_t flags = 0;
        char source_renderer[128]{};
        char worker_renderer[128]{};
        char worker_vendor[64]{};
        char worker_version[128]{};
    };

    static_assert(sizeof(platform_status) == 528, "PlatformStatus528");

}
