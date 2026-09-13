#pragma once
#include "linux_session_packets.h"
#include <cstdint>

namespace linux_session {

    enum capture_stage : uint32_t {
        capture_okay = 0,
        capture_cache_descriptor = 1,
        capture_hud_copy = 2,
        capture_world_dispatch = 3,
        capture_world_projection = 4,
        capture_world_model = 5,
        capture_world_root = 6,
        capture_world_copy = 7,
        capture_cache_affine = 8,
        capture_cache_copy = 9,
        capture_absent_world = 10,
        capture_frame_fence = 11,
        capture_pre_gui_copy = 12,
        capture_pre_gui_fence = 13,
        capture_scene_capacity = 14
    };

    // Facts recorded by one source copy. A word is meaningful only when its
    // validity bit in words[31] is set.
    struct copy_diagnostic {
        uint32_t words[32]{};
    };

    struct capture_diagnostic {
        uint32_t size = 960;
        uint32_t version = 1;
        uint32_t stage = 0;
        uint32_t render_thread = 0;
        uint64_t publication = 0;
        uint64_t published_ns = 0;
        uint64_t session = 0;
        uint64_t content_fence = 0;
        uint64_t active_generation = 0;
        uint64_t source_commit = 0;
        frame_packet frame{};
        cache_packet previous_cache{};
        double model[12]{}; // nominal affine a..f, x, z, size, ortho, width, height
        uint64_t model_revision = 0;
        int32_t model_map = -1;
        int32_t error = 0;
        copy_diagnostic copy{};
        pre_gui_packet pre_gui{};
        command_packet pending{};
    };

    static_assert(sizeof(capture_diagnostic) == 960, "CaptureDiagnostic960");

    inline bool remember_first_failure(capture_diagnostic& retained, const capture_diagnostic& observed) {
        if (retained.stage || !observed.stage || !observed.session) return false;
        retained = observed;
        return true;
    }

}
