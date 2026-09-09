#include "metal_owner.h"

namespace mac {
    namespace {

        const char* shader_source = R"metal(
#include <metal_stdlib>
using namespace metal;

struct vertex_out {
    float4 position [[position]];
};

struct uniforms {
    float4 map_x;
    float4 map_y;
    float2 size;
    uint flip;
    uint force_opaque;
    float4 solid_color;
};

// One triangle that covers the whole target.
vertex vertex_out vertex_main(uint index [[vertex_id]]) {
    float2 p = index == 0 ? float2(-1, -1) : index == 1 ? float2(3, -1) : float2(-1, 3);
    return {float4(p, 0, 1)};
}

fragment float4 fragment_main(vertex_out v [[stage_in]], constant uniforms& u [[buffer(0)]],
    texture2d<float> source [[texture(0)]], sampler sample [[sampler(0)]]) {
    if (u.force_opaque == 2) return u.solid_color;
    float2 q = float2(dot(float3(v.position.xy, 1), u.map_x.xyz), dot(float3(v.position.xy, 1), u.map_y.xyz));
    if (any(q < 0) || any(q >= u.size)) discard_fragment();
    if (u.flip) q.y = u.size.y - q.y;
    float4 c = source.sample(sample, q / u.size);
    if (u.force_opaque) c.a = 1;
    return c;
}
)metal";

        // Mirrors the MSL uniforms struct above.
        struct uniforms {
            float map_x[4], map_y[4], size[2];
            uint32_t flip, force_opaque;
            float solid_color[4];
        };
        static_assert(sizeof(uniforms) == 64, "Metal uniforms64");

    }

    int metal_worker::create(CAMetalLayer* target) {
        @autoreleasepool {
            if (!target || !target.device || queue) return E_INVALIDARG;

            layer = target;
            device = target.device;
            queue = [device newCommandQueue];
            NSError* error = nil;
            id<MTLLibrary> code = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil error:&error];
            if (!queue || !code || error) return E_FAIL;

            auto make = [&](bool blend) {
                MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [code newFunctionWithName:@"vertex_main"];
                d.fragmentFunction = [code newFunctionWithName:@"fragment_main"];
                d.colorAttachments[0].pixelFormat = layer.pixelFormat;
                d.colorAttachments[0].blendingEnabled = blend;
                d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                return [device newRenderPipelineStateWithDescriptor:d error:&error];
            };

            opaque = make(false);
            premult = make(true);

            auto sd = [MTLSamplerDescriptor new];
            sd.minFilter = MTLSamplerMinMagFilterNearest;
            sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler = [device newSamplerStateWithDescriptor:sd];

            return opaque && premult && sampler && !error ? 0 : E_FAIL;
        }
    }

    int metal_worker::draw(const image_layer& base, const image_layer& world, const image_layer& hud, const image_layer& cache,
        const affine& desired, std::shared_ptr<draw_completion> result, bool offscreen, const selection::geometry& selection_geometry) {
        @autoreleasepool {
            @try {
                if (!queue || !base.texture || !hud.texture || !result) return E_INVALIDARG;

                affine inverse;
                if (!invert_affine(desired, inverse)) return E_INVALIDARG;
                id<CAMetalDrawable> drawable = nil;
                id<MTLTexture> target = nil;

                if (offscreen) {
                    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat width:base.width height:base.height mipmapped:NO];
                    descriptor.storageMode = MTLStorageModePrivate;
                    descriptor.usage = MTLTextureUsageRenderTarget;
                    target = [device newTextureWithDescriptor:descriptor];
                } else {
                    drawable = [layer nextDrawable];
                    if (!drawable) return 1;
                    result->acquired_ns = native_now();
                    target = drawable.texture;
                }

                if (!target || target.width != base.width || target.height != base.height || target.device != device || target.pixelFormat != layer.pixelFormat) return 1;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                if (!cb) return E_FAIL;
                auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

                id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
                if (!encoder) return E_FAIL;

                auto draw_layer = [&](const image_layer& l, const affine& map, bool force_opaque) {
                    if (!l.texture) return;

                    uniforms u{{float(map.a), float(map.b), float(map.c), 0}, {float(map.d), float(map.e), float(map.f), 0},
                        {float(l.width), float(l.height)}, uint32_t(l.flip), uint32_t(force_opaque)};
                    [encoder setRenderPipelineState:force_opaque ? opaque : premult];
                    [encoder setFragmentTexture:l.texture atIndex:0];
                    [encoder setFragmentSamplerState:sampler atIndex:0];
                    [encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                };

                if (cache.texture) {
                    draw_layer(cache, affine{0, 0, .5, 0, 0, .5}, true);
                    draw_layer(cache, multiply_affine(cache.source, inverse), true);
                }

                draw_layer(base, world.texture ? multiply_affine(base.source, inverse) : affine{}, true);
                draw_layer(world, multiply_affine(world.source, inverse), false);

                if (!offscreen && selection_geometry.visible) {
                    uniforms u{};
                    u.force_opaque = 2;
                    const auto& c = selection_geometry.color;
                    for (int i = 0; i < 3; ++i) u.solid_color[i] = c[i] * c[3];
                    u.solid_color[3] = c[3];
                    [encoder setRenderPipelineState:premult];
                    [encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];

                    for (const auto& edge : selection_geometry.edges) {
                        NSUInteger l = NSUInteger(std::max(0.f, std::min(float(target.width), edge.left)));
                        NSUInteger r = NSUInteger(std::max(0.f, std::min(float(target.width), edge.right)));
                        NSUInteger t = NSUInteger(std::max(0.f, std::min(float(target.height), edge.top)));
                        NSUInteger b = NSUInteger(std::max(0.f, std::min(float(target.height), edge.bottom)));
                        if (r > l && b > t) {
                            [encoder setScissorRect:MTLScissorRect{l, t, r - l, b - t}];
                            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        }
                    }

                    [encoder setScissorRect:MTLScissorRect{0, 0, target.width, target.height}];
                }

                draw_layer(hud, affine{}, false);
                [encoder endEncoding];

                result->activation.bind(result->attempt, result->frame, result->generation, result->content, result->acquired_ns);
                uint64_t drawable_identity = (uint64_t)(__bridge void*)drawable;
                uint64_t command_identity = (uint64_t)(__bridge void*)cb;

                if (drawable) {
                    [drawable addPresentedHandler:^(id<MTLDrawable> actual) {
                        double time = actual.presentedTime;
                        if ((uint64_t)(__bridge void*)actual != drawable_identity || !std::isfinite(time) || time <= 0) {
                            result->activation.presented(false);
                            return;
                        }

                        result->presented_time = time;
                        result->presented_ns = native_now();
                        if (!result->presented.exchange(true, std::memory_order_acq_rel) && result->presentation) {
                            result->presentation->count.fetch_add(1, std::memory_order_relaxed);
                        }
                        result->activation.presented(true);
                    }];
                }

                MTLCommandBufferHandler callback = ^(id<MTLCommandBuffer> actual) {
                    result->result.store((uint64_t)(__bridge void*)actual == command_identity && actual.status == MTLCommandBufferStatusCompleted && !actual.error ? 0 : E_FAIL);
                    result->completed_ns = native_now();
                    result->completed.store(true, std::memory_order_release);
                    result->activation.gpu_completed(result->result.load() == 0);
                };

                [cb addCompletedHandler:callback];
                if (drawable) [cb presentDrawable:drawable];
                result->submission_attempted.store(true, std::memory_order_release);
                [cb commit];
                return 0;
            } @catch (NSException*) {
                return E_FAIL;
            }
        }
    }

    void metal_worker::release() {
        sampler = nil;
        opaque = nil;
        premult = nil;
        queue = nil;
        device = nil;
        layer = nil;
    }

}
