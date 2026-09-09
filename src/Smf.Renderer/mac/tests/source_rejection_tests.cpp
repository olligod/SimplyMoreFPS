#include "../source_rejection.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace present_observer;

source_rejection valid() {
    source_rejection x;
    x.target.width = 1280;
    x.target.height = 720;
    x.device = x.command_device = x.queue_device = x.source_device = x.original_device = 1;
    x.command = x.command_after = 2;
    x.queue = 3;
    x.before_pass = x.after_pass = 4;
    x.source_texture = x.before_texture = x.after_texture = 5;
    x.source_width = 1280;
    x.source_height = 720;
    x.source_format = x.original_format = 80;
    x.source_type = 2;
    x.sample_count = x.array_length = 1;

    return x;
}

// The boolean policy as it was before the per-reason diagnostic. Guards
// against a diagnostic change accidentally changing source eligibility.
bool original_reject(const source_rejection& x) {
    return !x.device || !x.command || x.command_device != x.device || !x.queue || x.queue_device != x.device ||
        x.source_device != x.device || x.command_after != x.command || x.command_status > 1 ||
        x.before_texture != x.source_texture || x.after_texture != x.source_texture ||
        x.after_level || x.after_slice || x.after_depth ||
        !(x.source_texture && x.source_type == 2 && x.sample_count == 1 && x.array_length == 1) ||
        x.framebuffer_only || x.source_width != x.target.width || x.source_height != x.target.height ||
        uint32_t(x.source_format) != uint32_t(x.original_format) || x.original_device != x.device;
}

int main() {
    static_assert(sizeof(source_rejection) == 400);
    assert(valid().size == 400 && reject_source(valid()) == source_reject::none);

    auto x = valid();
    x.before_pass = 0;
    x.before_texture = 0;
    assert(reject_source(x) == source_reject::pass_presence);
    x = valid();
    x.after_texture = 9;
    assert(reject_source(x) == source_reject::after_attachment);
    x = valid();
    x.source_width += 1ULL << 32;
    assert(reject_source(x) == source_reject::width);

    x = valid();
    x.after_level = 1ULL << 32;
    assert(reject_source(x) == source_reject::attachment_level);
    x = valid();
    x.source_type += 1ULL << 32;
    assert(reject_source(x) == source_reject::texture_type);
    x = valid();
    x.device = 0;
    x.after_texture = 9;
    assert(reject_source(x) == source_reject::no_device);

    x = valid();
    x.command_after = 0;
    x.source_width = 1;
    assert(reject_source(x) == source_reject::changed_command);
    x = valid();
    x.source_format += 1ULL << 32;
    assert(reject_source(x) == source_reject::none); // formats compare as uint32

    // A recorded frame: valid resolved source and an unchanged uncommitted
    // command buffer, no descriptor before or after TextureFromRenderBuffer.
    auto idle = valid();
    idle.before_pass = idle.after_pass = idle.before_texture = idle.after_texture = 0;
    idle.device = idle.command_device = idle.queue_device = idle.source_device = idle.original_device = 140460004466688ULL;
    idle.command = idle.command_after = 140460139126064ULL;
    idle.queue = 140459983838784ULL;
    idle.source_texture = 140459983793440ULL;
    idle.target.native_render_buffer = 4567473200ULL;
    assert(reject_source(idle) == source_reject::none);

    x = idle;
    x.after_pass = 4;
    assert(reject_source(x) == source_reject::pass_presence);
    x = idle;
    x.before_pass = 4;
    assert(reject_source(x) == source_reject::pass_presence);
    x = idle;
    x.before_texture = x.source_texture;
    assert(reject_source(x) == source_reject::before_attachment);
    x = idle;
    x.after_texture = x.source_texture;
    assert(reject_source(x) == source_reject::after_attachment);

    x = idle;
    x.before_level = 1;
    assert(reject_source(x) == source_reject::before_level);
    x = idle;
    x.before_slice = 1;
    assert(reject_source(x) == source_reject::before_slice);
    x = idle;
    x.before_depth = 1;
    assert(reject_source(x) == source_reject::before_depth);

    x = idle;
    x.command_status = 2;
    assert(reject_source(x) == source_reject::command_already_submitted);
    x = idle;
    x.command_after++;
    assert(reject_source(x) == source_reject::changed_command);
    x = idle;
    x.source_device++;
    assert(reject_source(x) == source_reject::source_device);
    x = idle;
    x.source_width++;
    assert(reject_source(x) == source_reject::width);

    x = idle;
    x.sample_count = 4;
    assert(reject_source(x) == source_reject::sample_count);
    x = idle;
    x.framebuffer_only = 1;
    assert(reject_source(x) == source_reject::framebuffer_only);
    x = idle;
    x.source_format = 81;
    assert(reject_source(x) == source_reject::pixel_format);

    x = valid();
    x.before_level = 1;
    assert(reject_source(x) == source_reject::before_level);
    x = valid();
    x.before_slice = 1;
    assert(reject_source(x) == source_reject::before_slice);
    x = valid();
    x.before_depth = 1;
    assert(reject_source(x) == source_reject::before_depth);

    // Every scalar the boolean policy looked at, mutated alone and in pairs.
    uint64_t source_rejection::*fields[] = {
        &source_rejection::device, &source_rejection::command, &source_rejection::command_device,
        &source_rejection::queue, &source_rejection::queue_device, &source_rejection::source_device,
        &source_rejection::command_after, &source_rejection::command_status,
        &source_rejection::before_texture, &source_rejection::after_texture,
        &source_rejection::after_level, &source_rejection::after_slice, &source_rejection::after_depth,
        &source_rejection::source_texture, &source_rejection::source_type, &source_rejection::sample_count,
        &source_rejection::array_length, &source_rejection::framebuffer_only,
        &source_rejection::source_width, &source_rejection::source_height,
        &source_rejection::source_format, &source_rejection::original_format, &source_rejection::original_device};

    const uint64_t values[] = {0, 1, 2, 3, 4, 80, 1280, 720, 1ULL << 32, UINT64_MAX};
    unsigned checks = 0;

    for (auto field : fields) {
        for (auto value : values) {
            x = valid();
            x.*field = value;
            assert((reject_source(x) != source_reject::none) == original_reject(x));
            ++checks;
        }
    }

    for (auto first : fields) {
        for (auto second : fields) {
            x = valid();
            x.*first = 0;
            x.*second = UINT64_MAX;
            assert((reject_source(x) != source_reject::none) == original_reject(x));
            ++checks;
        }
    }

    std::printf("Source rejection: %u mutations agree with the boolean policy; recorded nil/nil EOF and mixed cases PASS\n", checks);
}
