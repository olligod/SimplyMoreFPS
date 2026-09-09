#pragma once
#include "source_target.h"
#include <cstddef>
#include <cstdint>

namespace present_observer {

    // Numeric values are part of the diagnostic record; keep the order.
    enum class source_reject : uint32_t {
        none = 0, no_device, no_command, command_device, no_queue, queue_device, source_device,
        changed_command, command_already_submitted, before_attachment, after_attachment,
        attachment_level, attachment_slice, attachment_depth, no_source, texture_type, sample_count,
        array_length, framebuffer_only, width, height, pixel_format, original_device,
        queue_hook, source_lease, changed_command_after_end, queue_after_end, submitted_after_end, exception,
        pass_presence, before_level, before_slice, before_depth,
        track_buffer, texture_hook, missing_gpu_lease, source_array_full
    };

    // Diagnostic record of a rejected source. Every pointer is stored as a plain identity.
    struct source_rejection {
        uint32_t size = 400, version = 1, reason = 0, stage = 0;
        uint64_t failure_ordinal = 0, observed_ns = 0, render_thread = 0;
        mac_source_target target{};
        uint64_t restore_serial = 0;
        uint64_t device = 0, command = 0, command_after = 0, queue = 0, command_device = 0, queue_device = 0;
        uint64_t source_device = 0, original_device = 0, before_pass = 0, after_pass = 0, before_texture = 0, after_texture = 0;
        uint64_t source_texture = 0, original_layer = 0;
        uint64_t command_status = 0, source_format = 0, original_format = 0, source_width = 0, source_height = 0, source_type = 0;
        uint64_t sample_count = 0, array_length = 0, mipmap_levels = 0, storage_mode = 0, framebuffer_only = 0;
        uint64_t before_level = 0, before_slice = 0, before_depth = 0, after_level = 0, after_slice = 0, after_depth = 0, reserved = 0;
        uint64_t properties_read = 0, command_after_end = 0, queue_after_end = 0;
        uint64_t status_after_end = 0;
    };

    static_assert(sizeof(source_rejection) == 400, "Source rejection400");
    static_assert(offsetof(source_rejection, target) == 40, "Exact target at40");
    static_assert(offsetof(source_rejection, device) == 112, "Device at112");
    static_assert(offsetof(source_rejection, command_status) == 224, "Scalar metadata at224");
    static_assert(offsetof(source_rejection, properties_read) == 368, "Read facts at368");

    // The EOF may have no active encoder after the frame blit ended. The staged
    // renderbuffer stays the exact source; an existing pass must still agree.
    inline source_reject reject_source(const source_rejection& x) {
        if (!x.device) return source_reject::no_device;
        if (!x.command) return source_reject::no_command;
        if (x.command_device != x.device) return source_reject::command_device;
        if (!x.queue) return source_reject::no_queue;
        if (x.queue_device != x.device) return source_reject::queue_device;
        if (x.source_device != x.device) return source_reject::source_device;
        if (x.command_after != x.command) return source_reject::changed_command;
        if (x.command_status > 1) return source_reject::command_already_submitted;
        if (bool(x.before_pass) != bool(x.after_pass)) return source_reject::pass_presence;

        // nil/nil is coherent only with nil attachment facts. A live pass must name
        // this exact source at both observations.
        const uint64_t expected_attachment = x.before_pass ? x.source_texture : 0;
        if (x.before_texture != expected_attachment) return source_reject::before_attachment;
        if (x.after_texture != expected_attachment) return source_reject::after_attachment;
        if (x.before_level) return source_reject::before_level;
        if (x.before_slice) return source_reject::before_slice;
        if (x.before_depth) return source_reject::before_depth;
        if (x.after_level) return source_reject::attachment_level;
        if (x.after_slice) return source_reject::attachment_slice;
        if (x.after_depth) return source_reject::attachment_depth;

        if (!x.source_texture) return source_reject::no_source;
        if (x.source_type != 2) return source_reject::texture_type; // MTLTextureType2D
        if (x.sample_count != 1) return source_reject::sample_count;
        if (x.array_length != 1) return source_reject::array_length;
        if (x.framebuffer_only) return source_reject::framebuffer_only;
        if (x.source_width != x.target.width) return source_reject::width;
        if (x.source_height != x.target.height) return source_reject::height;
        if (uint32_t(x.source_format) != uint32_t(x.original_format)) return source_reject::pixel_format;
        if (x.original_device != x.device) return source_reject::original_device;

        return source_reject::none;
    }

}
