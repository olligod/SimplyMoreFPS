#pragma once
#include <stddef.h>
#include <stdint.h>

// Packets shared with the C# side; layouts are frozen. The *_ns fields sit where the
// Windows build keeps QPC stamps and carry CLOCK_MONOTONIC nanoseconds here.
namespace linux_session {

    // Scene probes may leave an offscreen FBO bound; the base copy still reads the original drawable.
    enum pre_gui_flags : uint32_t { pre_gui_map = 1, pre_gui_scene = 2 };

#pragma pack(push, 8)

    // The no-map case requires all-zero bytes.
    struct pose_packet {
        uint32_t size, version;
        uint64_t frame_id, unity_frame, camera_id, epoch;
        float x, y, z, orthographic_size;
        float pixel_x, pixel_y, pixel_width, pixel_height;
        float world_to_camera[16], projection[16];
        uint64_t camera_epoch, applied_sequence, model_revision;
        int32_t map_id;
        uint32_t reserved;
        double root_x, root_y, root_z, root_size;
    };

    struct command_packet {
        uint32_t size, version;
        uint64_t session, serial, generation, content_revision, previous_generation, after_frame;
        uint32_t operation, flags, width, height;
        float empty_background[4];
    };

    struct pre_gui_packet {
        uint32_t size, version;
        uint64_t session, content_revision, generation, source_frame;
        uint32_t width, height;
        uint64_t bootstrap_texture;
        uint32_t flags, reserved;
    };

    struct cache_packet {
        uint64_t texture, serial;
        uint32_t width, height, flags, reserved;
        // World X/Z to top-left cache texels (a, b, c, d, e, f), before any row flip.
        double affine[6];
    };

    struct frame_packet {
        uint32_t size, version;
        uint64_t session, content_revision, generation, source_frame;
        uint64_t world_texture, hud_texture;
        int64_t eof_ns;
        uint32_t flags, world_dispatches;
        pose_packet pose;
        cache_packet cache;
        uint64_t scene_description;
    };

    struct native_frame_packet {
        uint32_t size, version;
        uint64_t session, restore_serial, source_frame, content_revision, generation;
        uint32_t width, height, flags, reserved; // flags bit 0: frame expiry only, not a complete native frame
    };

    struct ack_packet {
        uint32_t size, version;
        uint64_t session, serial, generation, content_revision, source_frame;
        uint32_t operation, evidence;
        int32_t result;
        uint32_t disposition; // 1 success, 2 superseded before activation, 3 failure
        uint64_t commit_serial;
        int64_t completed_ns;
    };

    struct generation_status_packet {
        uint64_t generation, content_revision, last_source_frame, prepared_frame;
        uint64_t frames, source_commit, completed_commit, attachment, attachment_completed, retired_serial;
        uint32_t state, width, height, flags; // state: 0 empty, 1 preparing, 2 ready, 3 active, 4 retiring, 5 retired, 6 failed
        uint64_t retire_requested, staged_frame;
        uint32_t base_format, world_format, hud_format, reserved;
    };

    struct status_packet {
        uint32_t size, version;
        uint64_t session, content_fence, content_acknowledged, operation_fence, active_generation, active_frame, last_ack_serial;
        uint32_t main_thread, render_thread, worker_thread, completion_thread;
        uint32_t worker_state, stage;
        int32_t result;
        uint32_t flags; // bit 0 original window root, bit 1 native frame presented, bit 2 retained bundle
        uint64_t source_commit, source_completed, worker_commit, worker_completed;
        uint64_t dropped_frames, queued_callbacks;
        generation_status_packet generations[2];
        uint64_t native_ordered_frame, native_ordered_restore_serial, native_submitted_frame, native_reveal_frame;
        uint64_t native_submitted_generation, native_submitted_content, native_submitted_restore_serial;
        uint64_t native_present_serial, native_backbuffer;
    };

#pragma pack(pop)

    static_assert(sizeof(pose_packet) == 264 && offsetof(pose_packet, root_x) == 232, "Pose264");
    static_assert(sizeof(command_packet) == 88, "Command88");
    static_assert(sizeof(pre_gui_packet) == 64, "PreGui64");
    static_assert(sizeof(cache_packet) == 80 && offsetof(cache_packet, affine) == 32, "Cache80");
    static_assert(sizeof(frame_packet) == 424 && offsetof(frame_packet, pose) == 72 &&
                  offsetof(frame_packet, cache) == 336 && offsetof(frame_packet, scene_description) == 416, "Frame424 version4");
    static_assert(sizeof(native_frame_packet) == 64, "NativeFrame64");
    static_assert(sizeof(ack_packet) == 80, "Ack80");
    static_assert(sizeof(generation_status_packet) == 128, "Generation128");
    static_assert(sizeof(status_packet) == 472, "Status472");

}
