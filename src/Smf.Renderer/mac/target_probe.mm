#include "metal_owner.h"
#include "target_probe.h"
#include <limits>

namespace mac {
    namespace {

        template<class T> uint64_t identity(T value) { return (uint64_t)(__bridge void*)value; }

        void record_pass(MTLRenderPassDescriptor* pass, bool before, mac_target_probe_observation& o) {
            auto attachment = pass ? pass.colorAttachments[0] : nil;
            id<MTLTexture> texture = attachment.texture;

            if (before) {
                o.pass_before = identity(pass);
                o.pass_texture_before = identity(texture);
                o.load_before = uint32_t(attachment.loadAction);
                o.store_before = uint32_t(attachment.storeAction);
                o.pass_width_before = uint32_t(texture.width);
                o.pass_height_before = uint32_t(texture.height);
                o.pass_format_before = uint32_t(texture.pixelFormat);
            } else {
                o.pass_after = identity(pass);
                o.pass_texture_after = identity(texture);
                o.load_after = uint32_t(attachment.loadAction);
                o.store_after = uint32_t(attachment.storeAction);
                o.pass_width_after = uint32_t(texture.width);
                o.pass_height_after = uint32_t(texture.height);
                o.pass_format_after = uint32_t(texture.pixelFormat);
            }
        }

    }

    void observe_target(IUnityGraphicsMetal* metal, mac_target_probe_observation& o) {
        @autoreleasepool {
            o.render_thread = native_thread();
            o.render_ns = native_now();
            o.command_status_before = o.command_status_after = std::numeric_limits<uint32_t>::max();
            o.observer_sequence_before = drawable_observer::snapshot().last_sequence;

            @try {
                id<MTLDevice> device = metal->MetalDevice();
                o.metal_device = identity(device);
                id<MTLCommandBuffer> before = metal->CurrentCommandBuffer();
                o.command_before = identity(before);
                o.queue_before = identity(before.commandQueue);
                if (before) o.command_status_before = uint32_t(before.status);
                record_pass(metal->CurrentRenderPassDescriptor(), true, o);

                // These are the public native render-buffer and Metal APIs. A getter
                // can lazily establish engine state; the before/after facts expose
                // that possibility. This probe never publishes a source or an ack.
                id<MTLTexture> texture = metal->TextureFromRenderBuffer((UnityRenderBuffer)(uintptr_t)o.request.native_render_buffer);
                o.texture = identity(texture);
                o.texture_device = identity(texture.device);
                o.width = uint32_t(texture.width);
                o.height = uint32_t(texture.height);
                o.pixel_format = uint32_t(texture.pixelFormat);
                o.sample_count = uint32_t(texture.sampleCount);
                o.texture_type = uint32_t(texture.textureType);
                o.storage_mode = uint32_t(texture.storageMode);
                o.usage = uint32_t(texture.usage);
                o.framebuffer_only = texture.framebufferOnly ? 1u : 0u;

                id<MTLCommandBuffer> after = metal->CurrentCommandBuffer();
                o.command_after = identity(after);
                o.queue_after = identity(after.commandQueue);
                if (after) o.command_status_after = uint32_t(after.status);
                record_pass(metal->CurrentRenderPassDescriptor(), false, o);

                o.flags = (device ? 1u : 0u) | (before ? 2u : 0u) | (after ? 4u : 0u) | (texture ? 8u : 0u) |
                    (texture && texture.device == device ? 16u : 0u) |
                    (texture && o.width == o.request.width && o.height == o.request.height ? 32u : 0u) |
                    (before && before == after ? 64u : 0u) | (o.queue_before && o.queue_before == o.queue_after ? 128u : 0u);
                o.reason = !texture ? 1u : (!device || !before || !after) ? 2u : texture.device != device ? 3u :
                    (o.width != o.request.width || o.height != o.request.height) ? 4u : 0u;
            } @catch (NSException* exception) {
                (void)exception;
                o.reason = 5;
            }

            o.observer_sequence_after = drawable_observer::snapshot().last_sequence;
            if (o.observer_sequence_after != o.observer_sequence_before) o.flags |= 256u;
            o.checked_ns = native_now();
        }
    }

}
