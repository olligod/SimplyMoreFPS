#include "metal_owner.h"
#include "source_pass_policy.h"

namespace mac {

    int resolve_source_target(IUnityGraphicsMetal* metal, const mac_source_target& request, resolved_source_target& result) {
        @autoreleasepool {
            result = {};
            // Log cached scalar facts only; never call a getter again for diagnostics.
            uintptr_t device_id = 0, command_id = 0, queue_id = 0, before_id = 0, active_id = 0, texture_id = 0, after_id = 0, pass_id = 0, current_id = 0;
            uint64_t command_status = UINT64_MAX, after_status = UINT64_MAX, width = 0, height = 0, format = 0;
            int command_device = -1, queue_device = -1, after_device = -1, after_queue = -1, texture_device = -1;
            const char* stage = "arguments";

            auto reject = [&](const char* reason, int error = E_NOTIMPL) {
                NSLog(@"SMF source-target rejected reason=%s stage=%s session=%llu frame=%llu generation=%llu content=%llu renderBuffer=0x%llx requested=%ux%u actual=%llux%llu format=%llu device=0x%llx command=0x%llx queue=0x%llx beforePass=0x%llx beforeTexture=0x%llx source=0x%llx afterCommand=0x%llx afterPass=0x%llx afterTexture=0x%llx commandStatus=%llu afterStatus=%llu deviceMatches=%d/%d/%d/%d afterQueueMatch=%d",
                    reason, stage, (unsigned long long)request.session, (unsigned long long)request.source_frame,
                    (unsigned long long)request.generation, (unsigned long long)request.content_revision, (unsigned long long)request.native_render_buffer,
                    request.width, request.height, (unsigned long long)width, (unsigned long long)height, (unsigned long long)format,
                    (unsigned long long)device_id, (unsigned long long)command_id, (unsigned long long)queue_id,
                    (unsigned long long)before_id, (unsigned long long)active_id, (unsigned long long)texture_id,
                    (unsigned long long)after_id, (unsigned long long)pass_id, (unsigned long long)current_id,
                    (unsigned long long)command_status, (unsigned long long)after_status, command_device, queue_device, after_device, texture_device, after_queue);
                return error;
            };

            if (!metal || !valid_source_target(request)) return reject("arguments", E_INVALIDARG);

            @try {
                stage = "before-conversion";
                id<MTLDevice> device = metal->MetalDevice();
                device_id = (uintptr_t)(__bridge void*)device;
                id<MTLCommandBuffer> command = metal->CurrentCommandBuffer();
                command_id = (uintptr_t)(__bridge void*)command;
                id<MTLCommandQueue> queue = command.commandQueue;
                queue_id = (uintptr_t)(__bridge void*)queue;
                MTLRenderPassDescriptor* before = metal->CurrentRenderPassDescriptor();
                before_id = (uintptr_t)(__bridge void*)before;
                auto attachment = before ? before.colorAttachments[0] : nil;
                id<MTLTexture> active = attachment.texture;
                active_id = (uintptr_t)(__bridge void*)active;

                if (!device) return reject("no-device");
                if (!command) return reject("no-command");
                command_device = command.device == device;
                if (!command_device) return reject("command-device");
                if (!queue) return reject("no-queue");
                queue_device = queue.device == device;
                if (!queue_device) return reject("queue-device");
                command_status = command.status;
                if (command_status > MTLCommandBufferStatusEnqueued) return reject("command-submitted");

                const source_pass_facts before_facts{before != nil, active_id,
                    before ? uint64_t(attachment.level) : 0, before ? uint64_t(attachment.slice) : 0, before ? uint64_t(attachment.depthPlane) : 0};

                // Public Unity handle conversion. Never ask for a current encoder;
                // that getter can create one. This getter may establish engine state,
                // so the command identity is checked on both sides. A missing pass
                // means no current render encoder, not a missing source.
                stage = "conversion";
                id<MTLTexture> texture = metal->TextureFromRenderBuffer((UnityRenderBuffer)(uintptr_t)request.native_render_buffer);
                texture_id = (uintptr_t)(__bridge void*)texture;
                id<MTLCommandBuffer> after = metal->CurrentCommandBuffer();
                after_id = (uintptr_t)(__bridge void*)after;
                MTLRenderPassDescriptor* pass = metal->CurrentRenderPassDescriptor();
                pass_id = (uintptr_t)(__bridge void*)pass;
                auto current = pass ? pass.colorAttachments[0] : nil;

                stage = "after-conversion";
                if (after != command) return reject("changed-command");
                after_queue = after.commandQueue == queue;
                if (!after_queue) return reject("changed-queue");
                after_device = after.device == device;
                if (!after_device) return reject("after-device");
                after_status = after.status;
                if (after_status > MTLCommandBufferStatusEnqueued) return reject("after-submitted");

                id<MTLTexture> current_texture = current.texture;
                current_id = (uintptr_t)(__bridge void*)current_texture;
                const source_pass_facts after_facts{pass != nil, current_id,
                    pass ? uint64_t(current.level) : 0, pass ? uint64_t(current.slice) : 0, pass ? uint64_t(current.depthPlane) : 0};

                stage = "before-pass";
                if (const char* reason = reject_source_pass(before_facts, texture_id)) return reject(reason);
                stage = "after-pass";
                if (const char* reason = reject_source_pass(after_facts, texture_id)) return reject(reason);

                // The texture_valid contract, split into single-observation reasons.
                // The request extent was already checked by valid_source_target.
                stage = "texture";
                if (!texture) return reject("no-texture");
                texture_device = texture.device == device;
                if (!texture_device) return reject("texture-device");
                if (texture.textureType != MTLTextureType2D) return reject("texture-type");
                width = texture.width;
                if (width != request.width) return reject("width");
                height = texture.height;
                if (height != request.height) return reject("height");
                if (texture.sampleCount != 1) return reject("samples");
                if (texture.arrayLength != 1) return reject("array-length");
                if (texture.mipmapLevelCount != 1) return reject("mipmap-levels");
                if (texture.framebufferOnly) return reject("framebuffer-only");
                if (texture.storageMode == MTLStorageModeMemoryless) return reject("memoryless");
                format = texture.pixelFormat;
                if (format != MTLPixelFormatBGRA8Unorm && format != MTLPixelFormatRGBA8Unorm &&
                    format != MTLPixelFormatBGRA8Unorm_sRGB && format != MTLPixelFormatRGBA8Unorm_sRGB) return reject("pixel-format");

                // Strong local ownership carries the resolution into the slot lease.
                // The caller retains this source until the producer fence completes.
                result.texture = texture;
                result.command = command;
                return 0;
            } @catch (NSException*) {
                return reject("exception", E_FAIL);
            }
        }
    }

}
