#include "metal_owner.h"
#include "present_observer.h"

namespace mac {

    bool texture_valid(id<MTLTexture> t, id<MTLDevice> d, uint32_t w, uint32_t h, uint32_t f) {
        return t && d && t.device == d && t.textureType == MTLTextureType2D && t.width == w && t.height == h && w && h && w <= 16384 && h <= 16384 &&
            t.sampleCount == 1 && t.arrayLength == 1 && t.mipmapLevelCount == 1 && !t.framebufferOnly && t.storageMode != MTLStorageModeMemoryless &&
            (!f || t.pixelFormat == f) && (t.pixelFormat == MTLPixelFormatBGRA8Unorm || t.pixelFormat == MTLPixelFormatRGBA8Unorm ||
            t.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB || t.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB);
    }

    int encode_copies(IUnityGraphicsMetalV2* metal, copy_pair* copies, size_t count, std::shared_ptr<source_completion>& completed, id<MTLCommandBuffer> expected_command) {
        @autoreleasepool {
            if (!metal || !copies || !count || count > 4) return E_INVALIDARG;

            id<MTLDevice> device = metal->MetalDevice();
            id<MTLCommandBuffer> command = metal->CurrentCommandBuffer();

            if (!device || !command || (expected_command && command != expected_command) || command.device != device || !command.commandQueue ||
                command.status > MTLCommandBufferStatusEnqueued) return E_NOTIMPL;

            for (size_t i = 0; i < count; ++i) {
                auto& p = copies[i];
                if (!p.target || !texture_valid(p.source, device, p.target->width, p.target->height)) return E_INVALIDARG;

                // The caller reserved a retired slot. Replacing an old allocation is
                // safe only after both the producer and the consumer fences.
                if (p.target->texture && !texture_valid(p.target->texture, device, p.target->width, p.target->height, uint32_t(p.source.pixelFormat))) p.target->texture = nil;
                if (!p.target->texture) {
                    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:p.source.pixelFormat width:p.target->width height:p.target->height mipmapped:NO];
                    descriptor.storageMode = MTLStorageModePrivate;
                    descriptor.usage = MTLTextureUsageShaderRead;
                    p.target->texture = [device newTextureWithDescriptor:descriptor];
                }

                if (!texture_valid(p.target->texture, device, p.target->width, p.target->height, uint32_t(p.source.pixelFormat)) || p.target->texture == p.source) return E_FAIL;
            }

            auto result = std::make_shared<source_completion>();
            id<MTLBlitCommandEncoder> own = nil;
            id<MTLCommandBuffer> copy = nil;

            auto discard = [&] {
                if (result->disposition.commit_attempted) return false;
                if (!present_observer::discard_owned_unsubmitted_copy(copy)) return false;
                copy = nil;
                return result->disposition.discard();
            };

            @try {
                id<MTLCommandQueue> queue = metal->CommandQueue();
                if (!queue || queue != command.commandQueue || queue.device != device) return E_FAIL;

                const uint64_t producer_id = (uint64_t)(__bridge void*)command;
                const uint64_t queue_id = (uint64_t)(__bridge void*)queue;

                // Register before Unity submits. The producer and copy callbacks may
                // arrive in either order; publish only after both have finished.
                completed = result; // keeps producer ownership through every later exit
                [command addCompletedHandler:^(id<MTLCommandBuffer> actual) {
                    result->producer_result = (uint64_t)(__bridge void*)actual == producer_id && (uint64_t)(__bridge void*)actual.commandQueue == queue_id &&
                        actual.status == MTLCommandBufferStatusCompleted && !actual.error ? 0 : E_FAIL;
                    result->producer_done.store(true, std::memory_order_release);
                }];

                // EndCurrentCommandEncoder can leave Unity's blit encoder open. Let
                // Unity finish its whole buffer, including the encoder bookkeeping,
                // then copy on our own buffer. Never commit Unity's buffer ourselves.
                id<MTLCommandBuffer> producer = metal->CommitCurrentCommandBuffer();
                if (producer != command || producer.commandQueue != queue) {
                    discard();
                    return E_FAIL;
                }

                const auto producer_status = producer.status;
                if (producer_status == MTLCommandBufferStatusNotEnqueued) {
                    discard();
                    return 1;
                }
                if (producer_status == MTLCommandBufferStatusError || producer.error) {
                    discard();
                    return E_FAIL;
                }

                // Enqueued reserves the producer order even when Unity defers its
                // commit. Submit this copy before Unity's next rendering commands.
                copy = [queue commandBuffer];
                if (!copy || copy == producer || copy.commandQueue != queue || copy.device != device) {
                    if (copy == producer) copy = nil;
                    discard();
                    return E_FAIL;
                }

                result->command = (uint64_t)(__bridge void*)copy;
                result->queue = (uint64_t)(__bridge void*)queue;
                own = [copy blitCommandEncoder];
                if (!own) {
                    discard();
                    return E_FAIL;
                }

                for (size_t i = 0; i < count; ++i) {
                    auto& p = copies[i];
                    [own copyFromTexture:p.source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(p.target->width, p.target->height, 1) toTexture:p.target->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                }

                id<MTLBlitCommandEncoder> ending = own;
                own = nil;
                [ending endEncoding];

                MTLCommandBufferHandler callback = ^(id<MTLCommandBuffer> actual) {
                    result->result = (uint64_t)(__bridge void*)actual == result->command && (uint64_t)(__bridge void*)actual.commandQueue == result->queue &&
                        actual.status == MTLCommandBufferStatusCompleted && !actual.error ? 0 : E_FAIL;
                    result->completed_ns = native_now();
                    result->thread = native_thread();
                    result->done.store(true, std::memory_order_release);
                };
                [copy addCompletedHandler:callback];
                result->disposition.begin_commit();
                [copy commit];
                return 0;
            } @catch (NSException*) {
                if (own) {
                    id<MTLBlitCommandEncoder> ending = own;
                    own = nil;
                    @try {
                        [ending endEncoding];
                    } @catch (NSException*) {
                    }
                }

                // Already encoded resources stay quarantined with the caller; an
                // exception does not prove the GPU stopped referencing them.
                @try {
                    discard();
                } @catch (NSException*) {
                }

                completed = result;
                return E_FAIL;
            }
        }
    }

}
