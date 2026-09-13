#pragma once
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "platform.h"
#include "unity/IUnityGraphics.h"
#include "unity/IUnityGraphicsMetal.h"
#include "drawable_observer.h"
#include "activation.h"
#include "copy_disposition.h"
#include "../common/selection_overlay.h"
#include "../common/scene_snapshot.h"
#include "scene_compositor.h"

namespace mac {

    // One composited input image (base, world, hud or cache) copied out of Unity.
    struct image_layer {
        __strong id<MTLTexture> texture = nil;
        uint32_t width = 0, height = 0;
        bool flip = false;
        affine source{};
    };

    // Completion state of one source copy: Unity's producer buffer and our copy buffer.
    struct source_completion {
        copy_disposition disposition;
        std::atomic<bool> done{false}, producer_done{false};
        int result = 1, producer_result = 1;
        uint64_t command = 0, queue = 0, completed_ns = 0;
        uint32_t thread = 0;

        bool completed() const {
            return disposition.retired(producer_done.load(std::memory_order_acquire), done.load(std::memory_order_acquire));
        }

        bool succeeded() const {
            return producer_done.load(std::memory_order_acquire) && done.load(std::memory_order_acquire) &&
                disposition.usable(producer_result == 0, result == 0);
        }

        bool retired_without_failure() const {
            return completed() && producer_result == 0 && (disposition.discarded.load(std::memory_order_acquire) || result == 0);
        }
    };

    struct copy_pair {
        __strong id<MTLTexture> source = nil;
        image_layer* target = nullptr;
        bool scene_format = false;
    };

    struct captured_scene {
        smf_scene::snapshot packet{};
        std::array<image_layer, smf_scene::maximum_images> images{};
        std::array<std::shared_ptr<source_completion>, smf_scene::maximum_images> dependencies{};

        bool completed() const {
            for (uint32_t i = 0; i < packet.frame.image_count; ++i) {
                if (!dependencies[i] || !dependencies[i]->completed()) return false;
            }
            return true;
        }

        bool succeeded() const {
            for (uint32_t i = 0; i < packet.frame.image_count; ++i) {
                if (!dependencies[i] || !dependencies[i]->succeeded()) return false;
            }
            return true;
        }

        bool retired_without_failure() const {
            for (uint32_t i = 0; i < packet.frame.image_count; ++i) {
                if (!dependencies[i] || !dependencies[i]->retired_without_failure()) return false;
            }
            return true;
        }
    };

    struct resolved_source_target {
        __strong id<MTLTexture> texture = nil;
        __strong id<MTLCommandBuffer> command = nil;
    };

    int resolve_source_target(IUnityGraphicsMetal*, const mac_source_target&, resolved_source_target&);
    // Render callback only. Unity submits its producer buffer; a separate copy
    // buffer follows on the same queue before Unity resumes. A positive result
    // means the submission is not ordered yet and nothing was encoded.
    int encode_copies(IUnityGraphicsMetalV2*, copy_pair*, size_t count, std::shared_ptr<source_completion>&, id<MTLCommandBuffer> expected_command = nil);
    bool texture_valid(id<MTLTexture>, id<MTLDevice>, uint32_t width, uint32_t height, uint32_t expected_format = 0);
    bool scene_texture_valid(id<MTLTexture>, id<MTLDevice>, const smf_scene::image&);
    uint32_t texture_bytes(id<MTLTexture>);

    struct presentation_count {
        const uint64_t session, generation;
        std::atomic<uint64_t> count{0};

        presentation_count(uint64_t s, uint64_t g) : session(s), generation(g) {}
    };

    // Completion state of one compositor draw. Callbacks retain it, so an old
    // session can never increment a new counter.
    struct draw_completion {
        std::shared_ptr<presentation_count> presentation;
        std::shared_ptr<activation_attempt> attempt;
        activation_candidate activation;
        std::atomic<bool> completed{false}, presented{false}, submission_attempted{false};
        std::atomic<int> result{1};
        uint64_t serial = 0, frame = 0, generation = 0, content = 0;
        uint64_t acquired_ns = 0, completed_ns = 0, presented_ns = 0;
        double presented_time = 0;
    };

    // Owns the overlay CAMetalLayer inside Unity's window. AppKit main only.
    struct window_owner {
        __strong NSWindow* window = nil;
        __strong NSView* original_view = nil;
        __strong CAMetalLayer* original = nil;
        __strong CAMetalLayer* layer = nil;
        uint32_t number = 0, width = 0, height = 0, image_width = 0, image_height = 0;
        bool attached = false, visible = false;
        std::atomic<uint64_t> transaction_completed{0};
        std::atomic<uint64_t> scene_interval_ns{16666667};
        uint64_t transaction_issued = 0;

        int create(uint64_t window_address);
        int configure(id<MTLDevice>, uint32_t width, uint32_t height);
        int refresh_geometry();
        uint64_t show(bool);
        int remove();
    };

    struct metal_worker {
        selection::worker selection;
        scene_compositor scene;
        std::atomic<bool> scene_allocated{false};
        __strong CAMetalLayer* layer = nil;
        __strong id<MTLDevice> device = nil;
        __strong id<MTLCommandQueue> queue = nil;
        __strong id<MTLRenderPipelineState> opaque = nil, premult = nil;
        __strong id<MTLSamplerState> sampler = nil;

        int create(CAMetalLayer*);
        int draw(const image_layer& base, const image_layer& world, const image_layer& hud, const image_layer& cache,
            const affine& desired, std::shared_ptr<draw_completion>, bool offscreen = false,
            const selection::geometry& selection_geometry = {}, const captured_scene* captured = nullptr,
            const scene_view& scene_desired = {});
        void release(); // worker only, after its last GPU completion
    };

    // A presented native frame, from whichever observer is active. Never source completion.
    struct original_frame {
        uint64_t session = 0, frame = 0, generation = 0, content = 0, restore = 0, serial = 0, drawable = 0, acquired_ns = 0, presented_ns = 0;
    };

    bool original_observer_available();
    bool poll_original_frame(original_frame&);
    void arm_original_frame(const session_native_frame&);

}
