#include "scene_compositor.h"
#include "scene_shaders.h"
#include "platform.h"
#include "../common/projection_math.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

namespace mac {

    using namespace smf_scene;

    namespace {

        using texture_bindings = std::array<id<MTLTexture>, 64>;

        struct affine {
            double a, b, c, d, e, f;
        };

        struct background_constants {
            float projection[16]{};
            float inverse_projection[16]{};
            float delta[4]{};
            float size[4]{};
        };

        struct parallax_constants {
            float x[4]{};
            float y[4]{};
            float size[4]{};
            uint32_t flags[4]{};
            float inverse_x[4]{};
            float inverse_z[4]{};
            float live_x[4]{};
            float live_y[4]{};
            float depth[4]{};
        };

        struct constants {
            float rows[4][4]{};
            float dimensions[2][4]{};
            uint32_t image_flags[4][4]{};
            float values[4]{};
            float inverse_projection[16]{};
            float projection[16]{};
            float camera_delta[4]{};
            background_constants backgrounds[4]{};
            parallax_constants parallax[16]{};
            float live_inverse_x[4]{};
            float live_inverse_z[4]{};
            float live_depth[4]{};
            uint32_t glow_counts[4]{};
            background_glow live_glows[maximum_background_glows]{};
            background_glow cached_glows[maximum_background_glows]{};
        };
        static_assert(sizeof(constants) == 3840, "Scene shader constant layout");

        bool finite(const double* values, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                if (!std::isfinite(values[i])) return false;
            }
            return true;
        }

        bool invert_matrix(const float* matrix, float* output) {
            double rows[4][8]{};
            for (size_t row = 0; row < 4; ++row) {
                for (size_t column = 0; column < 4; ++column) {
                    if (!std::isfinite(matrix[column * 4 + row])) return false;
                    rows[row][column] = matrix[column * 4 + row];
                }
                rows[row][row + 4] = 1;
            }
            for (size_t column = 0; column < 4; ++column) {
                size_t pivot = column;
                for (size_t row = column + 1; row < 4; ++row) {
                    if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
                }
                if (std::abs(rows[pivot][column]) < 1e-12) return false;
                for (size_t i = 0; i < 8; ++i) std::swap(rows[column][i], rows[pivot][i]);
                const double divisor = rows[column][column];
                for (size_t i = 0; i < 8; ++i) rows[column][i] /= divisor;
                for (size_t row = 0; row < 4; ++row) {
                    if (row == column) continue;
                    const double scale = rows[row][column];
                    for (size_t i = 0; i < 8; ++i) rows[row][i] -= rows[column][i] * scale;
                }
            }
            for (size_t row = 0; row < 4; ++row) {
                for (size_t column = 0; column < 4; ++column) {
                    output[column * 4 + row] = static_cast<float>(rows[row][column + 4]);
                    if (!std::isfinite(output[column * 4 + row])) return false;
                }
            }
            return true;
        }

        bool sampling_rows(const layer& source, const scene_view& desired, const image& size, float* x, float* y) {
            if (!finite(source.affine, 6) || !finite(desired.map_affine, 6)) return false;
            const auto& m = desired.map_affine;
            const double determinant = m[0] * m[4] - m[1] * m[3];
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return false;
            affine transform = smf_projection::inverse(affine{m[0], m[1], m[2], m[3], m[4], m[5]}, determinant);
            if (source.kind == parallax_map) {
                transform.c -= (desired.camera_x - source.source_x) * source.parallax_x;
                transform.f -= (desired.camera_z - source.source_z) * source.parallax_z;
            }
            const auto& a = source.affine;
            transform = smf_projection::multiply(affine{a[0], a[1], a[2], a[3], a[4], a[5]}, transform);
            x[0] = static_cast<float>(transform.a / size.width);
            x[1] = static_cast<float>(transform.b / size.width);
            x[2] = static_cast<float>(transform.c / size.width);
            y[0] = static_cast<float>(transform.d / size.height);
            y[1] = static_cast<float>(transform.e / size.height);
            y[2] = static_cast<float>(transform.f / size.height);
            for (size_t i = 0; i < 3; ++i) {
                if (!std::isfinite(x[i]) || !std::isfinite(y[i])) return false;
            }
            return true;
        }

        bool inverse_rows(const layer& source, float* x, float* z) {
            const auto& a = source.affine;
            const double determinant = a[0] * a[4] - a[1] * a[3];
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return false;
            const affine inverse = smf_projection::inverse(affine{a[0], a[1], a[2], a[3], a[4], a[5]}, determinant);
            x[0] = static_cast<float>(inverse.a);
            x[1] = static_cast<float>(inverse.b);
            x[2] = static_cast<float>(inverse.c);
            z[0] = static_cast<float>(inverse.d);
            z[1] = static_cast<float>(inverse.e);
            z[2] = static_cast<float>(inverse.f);
            for (size_t i = 0; i < 3; ++i) {
                if (!std::isfinite(x[i]) || !std::isfinite(z[i])) return false;
            }
            return true;
        }

        bool depth_parameters(const layer& source, float* output) {
            output[0] = static_cast<float>(source.depth_x);
            output[1] = static_cast<float>(source.depth_z);
            output[2] = static_cast<float>(source.depth_scale);
            output[3] = static_cast<float>(source.depth_offset);
            for (size_t i = 0; i < 4; ++i) {
                if (!std::isfinite(output[i])) return false;
            }
            return std::abs(output[2]) > 1e-8;
        }

    }

    struct scene_compositor::state {
        struct target {
            __strong id<MTLTexture> texture = nil;
        };

        struct depth_target {
            __strong id<MTLTexture> texture = nil;
            uint32_t width = 0, height = 0;
        };

        struct depth_cache {
            struct completion {
                std::atomic<bool> succeeded{false};
            };

            std::array<depth_target, 4> levels{};
            __weak id<MTLCommandBuffer> producer = nil;
            __weak id<MTLTexture> input = nil;
            std::shared_ptr<completion> receipt;
            uint32_t level_count = 0;
            uint64_t serial = 0, source_identity = 0;
            uint32_t width = 0, height = 0, flags = 0;
        };

        __strong id<MTLDevice> device = nil;
        __weak id<MTLCommandBuffer> command = nil;
        __strong id<MTLRenderPipelineState> background = nil, layer = nil, parallax = nil;
        __strong id<MTLRenderPipelineState> correction = nil, additive = nil;
        __strong id<MTLComputePipelineState> reduce_depth = nil;
        __strong id<MTLSamplerState> point_sampler = nil, linear_sampler = nil;
        std::array<target, 2> targets{};
        std::array<depth_cache, 4> depth_bounds{};
        uint32_t width = 0, height = 0, current = 0;
        bool output_valid = false;

        int update_depth(uint32_t index, const image& metadata, id<MTLTexture> source, uint32_t flags) {
            auto& cache = depth_bounds[index];
            if (!metadata.serial) return E_INVALIDARG;
            const bool ordered = cache.producer == command || (cache.receipt &&
                cache.receipt->succeeded.load(std::memory_order_acquire));
            if (ordered && cache.input == source && cache.source_identity == metadata.texture && cache.serial == metadata.serial && cache.width == metadata.width &&
                cache.height == metadata.height && cache.flags == flags) return 0;

            cache.serial = 0;
            if (cache.width != metadata.width || cache.height != metadata.height || !cache.level_count) {
                std::array<depth_target, 4> next{};
                uint32_t count = 0;
                uint32_t w = metadata.width, h = metadata.height;
                do {
                    if (count == next.size()) return E_INVALIDARG;
                    w = (w + 15) / 16;
                    h = (h + 15) / 16;
                    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                        width:w height:h mipmapped:NO];
                    descriptor.storageMode = MTLStorageModePrivate;
                    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    auto& item = next[count++];
                    item.width = w;
                    item.height = h;
                    item.texture = [device newTextureWithDescriptor:descriptor];
                    if (!item.texture) return E_FAIL;
                } while (w > 1 || h > 1);
                cache.levels = std::move(next);
                cache.level_count = count;
                cache.width = metadata.width;
                cache.height = metadata.height;
            }

            constants data{};
            data.dimensions[0][0] = static_cast<float>(metadata.width);
            data.dimensions[0][1] = static_cast<float>(metadata.height);
            data.values[2] = 1;
            data.values[3] = (flags & negative_clip_depth) != 0 ? 1.f : 0.f;
            id<MTLTexture> original = source;
            for (uint32_t i = 0; i < cache.level_count; ++i) {
                auto& level = cache.levels[i];
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                if (!encoder) return E_FAIL;
                [encoder setComputePipelineState:reduce_depth];
                [encoder setBytes:&data length:sizeof(data) atIndex:0];
                [encoder setTexture:source atIndex:0];
                [encoder setTexture:level.texture atIndex:1];
                [encoder dispatchThreadgroups:MTLSizeMake(level.width, level.height, 1)
                    threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [encoder endEncoding];
                source = level.texture;
                data.dimensions[0][0] = static_cast<float>(level.width);
                data.dimensions[0][1] = static_cast<float>(level.height);
                data.values[2] = 0;
            }
            cache.serial = metadata.serial;
            cache.source_identity = metadata.texture;
            cache.flags = flags;
            cache.input = original;
            cache.producer = command;
            cache.receipt = std::make_shared<depth_cache::completion>();
            const auto receipt = cache.receipt;
            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                receipt->succeeded.store(completed.status == MTLCommandBufferStatusCompleted && !completed.error,
                    std::memory_order_release);
            }];
            return 0;
        }

        int create_targets(uint32_t w, uint32_t h) {
            if (width == w && height == h) return 0;
            auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                width:w height:h mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            std::array<target, 2> next{};
            for (auto& item : next) {
                item.texture = [device newTextureWithDescriptor:descriptor];
                if (!item.texture) return E_FAIL;
            }
            targets = std::move(next);
            width = w;
            height = h;
            return 0;
        }

        int run(id<MTLRenderPipelineState> pipeline, const constants& data,
            const texture_bindings& images, bool first = false) {
            const uint32_t next = first ? 0 : 1 - current;
            auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = targets[next].texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return E_FAIL;
            [encoder setRenderPipelineState:pipeline];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setViewport:MTLViewport{0, 0, double(width), double(height), 0, 1}];
            [encoder setFragmentBytes:&data length:sizeof(data) atIndex:0];
            texture_bindings bindings = images;
            id<MTLTexture> fallback = nil;
            for (auto image : images) {
                if (image) {
                    fallback = image;
                    break;
                }
            }
            if (!fallback) {
                [encoder endEncoding];
                return E_INVALIDARG;
            }
            for (auto& image : bindings) {
                if (!image) image = fallback;
            }
            [encoder setFragmentTextures:bindings.data() withRange:NSMakeRange(0, bindings.size())];
            for (NSUInteger i = 0; i < 16; ++i) {
                const uint32_t flags = data.image_flags[i / 4][i % 4];
                id<MTLSamplerState> sampler = i == 14 || (i < 11 && (flags & linear_filter)) ? linear_sampler : point_sampler;
                [encoder setFragmentSamplerState:sampler atIndex:i];
            }
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            current = next;
            return 0;
        }
    };

    scene_compositor::scene_compositor() = default;
    scene_compositor::~scene_compositor() = default;

    int scene_compositor::initialize(id<MTLDevice> device) {
        if (state_) return state_->device == device ? 0 : E_INVALIDARG;
        if (!device) return E_INVALIDARG;
        std::unique_ptr<state> next(new (std::nothrow) state);
        if (!next) return E_FAIL;
        next->device = device;
        NSError* error = nil;
        auto options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        id<MTLLibrary> code = [device newLibraryWithSource:[NSString stringWithUTF8String:scene_shader_source]
            options:options error:&error];
        if (!code || error) return E_FAIL;
        id<MTLFunction> vertex = [code newFunctionWithName:@"sceneVertex"];
        const char* names[] = {"backgroundPixel", "layerPixel", "parallaxPixel", "correctionPixel", "additivePixel"};
        id<MTLRenderPipelineState> __strong* pipelines[] = {&next->background, &next->layer, &next->parallax, &next->correction, &next->additive};
        for (size_t i = 0; i < std::size(names); ++i) {
            auto descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = vertex;
            descriptor.fragmentFunction = [code newFunctionWithName:[NSString stringWithUTF8String:names[i]]];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
            *pipelines[i] = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            if (!*pipelines[i] || error) return E_FAIL;
        }
        next->reduce_depth = [device newComputePipelineStateWithFunction:[code newFunctionWithName:@"reduceDepth"] error:&error];
        if (!next->reduce_depth || error || next->reduce_depth.maxTotalThreadsPerThreadgroup < 256) return E_FAIL;
        auto sampler = [MTLSamplerDescriptor new];
        sampler.sAddressMode = sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterNearest;
        next->point_sampler = [device newSamplerStateWithDescriptor:sampler];
        sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
        next->linear_sampler = [device newSamplerStateWithDescriptor:sampler];
        if (!next->point_sampler || !next->linear_sampler) return E_FAIL;
        state_ = std::move(next);
        return 0;
    }

    int scene_compositor::draw(id<MTLCommandBuffer> command, const scene_frame& source, const scene_view& desired) {
        if (!state_) return E_UNEXPECTED;
        auto& state = *state_;
        if (!command || command.device != state.device) return E_INVALIDARG;
        state.command = command;
        state.output_valid = false;
        if (!source.scene || !source.images || !source.layers || !source.resources ||
            !desired.width || !desired.height || desired.width > 16384 ||
            desired.height > 16384 || !std::isfinite(desired.camera_x) ||
            !std::isfinite(desired.camera_z)) return E_INVALIDARG;
        const auto& scene = *source.scene;
        if (scene.size != sizeof(description) || scene.version != version ||
            scene.image_count > maximum_images || scene.layer_count > maximum_layers ||
            scene.effect_count > maximum_effects || (scene.effect_count && !source.effects)) return E_INVALIDARG;
        const auto valid_image = [&](uint32_t index) {
            return index < scene.image_count && source.resources[index] &&
                source.images[index].width && source.images[index].height &&
                source.images[index].width <= 16384 &&
                source.images[index].height <= 16384 &&
                !(source.images[index].flags & ~(flip_y | linear_filter | depth_image | reversed_depth));
        };
        const auto same_size = [&](uint32_t first, uint32_t second) {
            return valid_image(first) && valid_image(second) &&
                source.images[first].width == source.images[second].width &&
                source.images[first].height == source.images[second].height;
        };
        const auto& world = scene.world;
        if (!valid_image(world.live_color) || !valid_image(world.sky) || !world.view_count || world.view_count > 4 ||
            (world.flags & ~negative_clip_depth) || !finite(&world.source_x, 10) ||
            world.min_x > world.max_x || world.min_z > world.max_z || world.sky_scale < 1 || world.reserved0 || world.reserved1 ||
            world.live_glow_count > maximum_background_glows || world.cached_glow_count > maximum_background_glows)
            return E_INVALIDARG;
        for (uint32_t group = 0; group < 2; ++group) {
            const auto* glows = group ? world.cached_glows : world.live_glows;
            const uint32_t count = group ? world.cached_glow_count : world.live_glow_count;
            for (uint32_t i = 0; i < count; ++i) {
                float fields[16]{};
                std::memcpy(fields, &glows[i], sizeof(fields));
                for (float field : fields) if (!std::isfinite(field)) return E_INVALIDARG;
                if (glows[i].planet_radius <= 0 || glows[i].glow_radius <= 0 || glows[i].mesh_radius <= 0)
                    return E_INVALIDARG;
            }
        }
        for (uint32_t i = 0; i < world.view_count; ++i) {
            const auto& captured = world.views[i];
            if (!same_size(captured.color, captured.depth) || captured.flags || captured.reserved ||
                !std::isfinite(captured.x) || !std::isfinite(captured.z)) return E_INVALIDARG;
        }
        const layer* live = nullptr;
        const layer* cache = nullptr;
        const layer* live_parallax = nullptr;
        const layer* parallax_projection = nullptr;
        uint32_t parallax_depth_order = 0;
        bool has_parallax_depth = false;
        for (uint32_t i = 0; i < scene.layer_count; ++i) {
            const auto& layer = source.layers[i];
            if (!same_size(layer.color, layer.probe) || !finite(layer.affine, 6) ||
                !finite(&layer.source_x, 4)) return E_INVALIDARG;
            if (layer.kind == live_map) {
                if (live || !same_size(layer.color, layer.reference)) return E_INVALIDARG;
                live = &layer;
            } else if (layer.kind == cached_map) {
                if (cache) return E_INVALIDARG;
                cache = &layer;
            } else if (layer.kind == parallax_map) {
                if (layer.flags != depth_occlusion || layer.reference != no_image || !same_size(layer.color, layer.depth) ||
                    !(source.images[layer.depth].flags & depth_image)) return E_INVALIDARG;
                const uint32_t order = source.images[layer.depth].flags & reversed_depth;
                if (has_parallax_depth && order != parallax_depth_order) return E_INVALIDARG;
                if (parallax_projection && (layer.depth_x != parallax_projection->depth_x ||
                    layer.depth_z != parallax_projection->depth_z || layer.depth_scale != parallax_projection->depth_scale ||
                    layer.depth_offset != parallax_projection->depth_offset ||
                    source.images[layer.depth].serial != source.images[parallax_projection->depth].serial)) return E_INVALIDARG;
                parallax_projection = &layer;
                parallax_depth_order = order;
                has_parallax_depth = true;
            } else if (layer.kind == live_parallax_map) {
                if (live_parallax || layer.flags != depth_occlusion || !same_size(layer.color, layer.reference) ||
                    !same_size(layer.color, layer.depth) || !(source.images[layer.depth].flags & depth_image)) return E_INVALIDARG;
                live_parallax = &layer;
            } else {
                return E_INVALIDARG;
            }
        }
        if (!live || !cache) return E_INVALIDARG;
        for (uint32_t i = 0; i < scene.effect_count; ++i) {
            const auto& effect = source.effects[i];
            if (effect.kind == image_filter) {
                return E_NOTIMPL;
            }
            if (!valid_image(effect.first_image) ||
                (effect.kind != color_correction && effect.kind != additive_image)) return E_INVALIDARG;
            if (effect.kind == color_correction &&
                (source.images[effect.first_image].width != 256 || source.images[effect.first_image].height != 4 ||
                !std::isfinite(effect.parameters[0]))) return E_INVALIDARG;
            if (effect.kind == additive_image &&
                (!same_size(effect.first_image, live->color) || !same_size(effect.second_image, cache->color))) return E_INVALIDARG;
        }

        constants data{};
        if (!invert_matrix(world.projection, data.inverse_projection)) return E_INVALIDARG;
        std::memcpy(data.projection, world.projection, sizeof(data.projection));
        data.values[0] = static_cast<float>(world.view_count);
        data.values[1] = static_cast<float>(world.sky_scale);
        data.values[3] = (world.flags & negative_clip_depth) != 0 ? 1.f : 0.f;
        data.glow_counts[0] = world.live_glow_count;
        data.glow_counts[1] = world.cached_glow_count;
        std::memcpy(data.live_glows, world.live_glows, sizeof(data.live_glows));
        std::memcpy(data.cached_glows, world.cached_glows, sizeof(data.cached_glows));
        const double camera_x = std::clamp(desired.camera_x, world.min_x, world.max_x);
        const double camera_z = std::clamp(desired.camera_z, world.min_z, world.max_z);
        data.camera_delta[0] = static_cast<float>((camera_x - std::clamp(world.source_x, world.min_x, world.max_x)) * world.camera_x_per_cell);
        data.camera_delta[1] = static_cast<float>((camera_z - std::clamp(world.source_z, world.min_z, world.max_z)) * world.camera_y_per_cell);
        data.values[2] = data.camera_delta[0] == 0 && data.camera_delta[1] == 0 ? 1.f : 0.f;
        for (float value : data.camera_delta) if (!std::isfinite(value)) return E_INVALIDARG;
        data.image_flags[0][1] = source.images[world.live_color].flags;
        data.image_flags[0][2] = source.images[world.sky].flags;

        int result = state.create_targets(desired.width, desired.height);
        if (FAILED(result)) return result;
        texture_bindings images{};
        images[1] = source.resources[world.live_color];
        images[2] = source.resources[world.sky];
        std::array<uint32_t, 4> view_order{0, 1, 2, 3};
        const auto distance = [&](uint32_t index) {
            const double x = (camera_x - world.views[index].x) * world.camera_x_per_cell;
            const double z = (camera_z - world.views[index].z) * world.camera_y_per_cell;
            return x * x + z * z;
        };
        std::sort(view_order.begin(), view_order.begin() + world.view_count,
            [&](uint32_t a, uint32_t b) { return distance(a) != distance(b) ? distance(a) < distance(b) : a < b; });
        for (uint32_t i = 0; i < world.view_count; ++i) {
            const uint32_t index = view_order[i];
            const auto& captured = world.views[index];
            auto& capture_data = data.backgrounds[i];
            if (!invert_matrix(captured.projection, capture_data.inverse_projection)) return E_INVALIDARG;
            std::memcpy(capture_data.projection, captured.projection, sizeof(capture_data.projection));
            capture_data.delta[0] = static_cast<float>((camera_x - std::clamp(captured.x, world.min_x, world.max_x)) * world.camera_x_per_cell);
            capture_data.delta[1] = static_cast<float>((camera_z - std::clamp(captured.z, world.min_z, world.max_z)) * world.camera_y_per_cell);
            if (!std::isfinite(capture_data.delta[0]) || !std::isfinite(capture_data.delta[1])) return E_INVALIDARG;
            const auto& depth = source.images[captured.depth];
            capture_data.size[0] = static_cast<float>(depth.width);
            capture_data.size[1] = static_cast<float>(depth.height);
            const uint32_t color_slot = 3 + i * 2;
            const uint32_t depth_slot = color_slot + 1;
            images[color_slot] = source.resources[captured.color];
            images[depth_slot] = source.resources[captured.depth];
            data.image_flags[color_slot / 4][color_slot % 4] = source.images[captured.color].flags;
            data.image_flags[depth_slot / 4][depth_slot % 4] = depth.flags;
            result = state.update_depth(index, depth, source.resources[captured.depth], world.flags);
            if (FAILED(result)) return result;
            const auto& bounds = state.depth_bounds[index];
            images[11 + i] = bounds.levels[bounds.level_count - 1].texture;
        }
        result = state.run(state.background, data, images, true);
        if (FAILED(result)) return result;

        constants parallax_data{};
        texture_bindings parallax_images{};
        parallax_images[0] = state.targets[state.current].texture;
        if (live_parallax) {
            const auto& color = source.images[live_parallax->color];
            parallax_data.values[1] = 1;
            parallax_data.dimensions[0][0] = static_cast<float>(color.width);
            parallax_data.dimensions[0][1] = static_cast<float>(color.height);
            const uint32_t indices[] = {live_parallax->color, live_parallax->probe, live_parallax->reference, live_parallax->depth};
            for (uint32_t i = 0; i < 4; ++i) {
                parallax_images[1 + i] = source.resources[indices[i]];
                parallax_data.image_flags[(i + 1) / 4][(i + 1) % 4] = source.images[indices[i]].flags;
            }
            if (!inverse_rows(*live_parallax, parallax_data.live_inverse_x, parallax_data.live_inverse_z) ||
                !depth_parameters(*live_parallax, parallax_data.live_depth)) return E_INVALIDARG;
        }
        uint32_t parallax_count = 0;
        for (uint32_t i = 0; i < scene.layer_count; ++i) {
            const auto& layer = source.layers[i];
            if (layer.kind != parallax_map) continue;
            auto& fields = parallax_data.parallax[parallax_count];
            const auto& color = source.images[layer.color];
            if (!sampling_rows(layer, desired, color, fields.x, fields.y)) return E_INVALIDARG;
            fields.size[0] = static_cast<float>(color.width);
            fields.size[1] = static_cast<float>(color.height);
            fields.flags[0] = color.flags;
            fields.flags[1] = source.images[layer.probe].flags;
            fields.flags[2] = source.images[layer.depth].flags;
            if (!inverse_rows(layer, fields.inverse_x, fields.inverse_z) || !depth_parameters(layer, fields.depth)) return E_INVALIDARG;
            if (live_parallax) {
                auto live_projection = *live_parallax;
                live_projection.kind = parallax_map;
                live_projection.parallax_x = layer.parallax_x;
                live_projection.parallax_z = layer.parallax_z;
                if (!sampling_rows(live_projection, desired, source.images[live_parallax->color], fields.live_x, fields.live_y)) return E_INVALIDARG;
            }
            const uint32_t slot = 16 + parallax_count * 3;
            parallax_images[slot] = source.resources[layer.color];
            parallax_images[slot + 1] = source.resources[layer.probe];
            parallax_images[slot + 2] = source.resources[layer.depth];
            ++parallax_count;
        }
        if (parallax_count) {
            parallax_data.values[0] = static_cast<float>(parallax_count);
            result = state.run(state.parallax, parallax_data, parallax_images);
            if (FAILED(result)) return result;
        }

        const auto draw_layer = [&](const smf_scene::layer* foreground, const smf_scene::layer& cached) {
            constants layer_data{};
            texture_bindings bindings{};
            bindings[0] = state.targets[state.current].texture;
            bindings[4] = source.resources[cached.color];
            bindings[5] = source.resources[cached.probe];
            layer_data.image_flags[1][0] = source.images[cached.color].flags;
            layer_data.image_flags[1][1] = source.images[cached.probe].flags;
            layer_data.dimensions[1][0] = static_cast<float>(source.images[cached.color].width);
            layer_data.dimensions[1][1] = static_cast<float>(source.images[cached.color].height);
            if (!sampling_rows(cached, desired, source.images[cached.color], layer_data.rows[2], layer_data.rows[3])) return E_INVALIDARG;
            if (foreground) {
                layer_data.values[0] = 1;
                layer_data.values[1] = foreground->source_x == desired.camera_x && foreground->source_z == desired.camera_z &&
                    source.images[foreground->color].width == desired.width && source.images[foreground->color].height == desired.height &&
                    std::equal(foreground->affine, foreground->affine + 6, desired.map_affine) ? 1.f : 0.f;
                bindings[1] = source.resources[foreground->color];
                bindings[2] = source.resources[foreground->probe];
                bindings[3] = source.resources[foreground->reference];
                layer_data.image_flags[0][1] = source.images[foreground->color].flags;
                layer_data.image_flags[0][2] = source.images[foreground->probe].flags;
                layer_data.image_flags[0][3] = source.images[foreground->reference].flags;
                layer_data.dimensions[0][0] = static_cast<float>(source.images[foreground->color].width);
                layer_data.dimensions[0][1] = static_cast<float>(source.images[foreground->color].height);
                if (!sampling_rows(*foreground, desired, source.images[foreground->color], layer_data.rows[0], layer_data.rows[1])) return E_INVALIDARG;
            }
            return state.run(state.layer, layer_data, bindings);
        };
        result = draw_layer(live, *cache);
        if (FAILED(result)) return result;

        for (uint32_t i = 0; i < scene.effect_count; ++i) {
            const auto& effect = source.effects[i];
            constants effect_data{};
            effect_data.values[0] = effect.parameters[0];
            effect_data.image_flags[0][1] = source.images[effect.first_image].flags;
            texture_bindings bindings{};
            bindings[0] = state.targets[state.current].texture;
            bindings[1] = source.resources[effect.first_image];
            id<MTLRenderPipelineState> shader = state.correction;
            if (effect.kind == additive_image) {
                shader = state.additive;
                bindings[4] = source.resources[effect.second_image];
                effect_data.image_flags[1][0] = source.images[effect.second_image].flags;
                if (!sampling_rows(*live, desired, source.images[effect.first_image], effect_data.rows[0], effect_data.rows[1])) return E_INVALIDARG;
                if (!sampling_rows(*cache, desired, source.images[effect.second_image], effect_data.rows[2], effect_data.rows[3])) return E_INVALIDARG;
            }
            result = state.run(shader, effect_data, bindings);
            if (FAILED(result)) return result;
        }
        state.output_valid = true;
        return 0;
    }

    id<MTLTexture> scene_compositor::texture() const {
        return state_ && state_->output_valid ? state_->targets[state_->current].texture : nil;
    }

    void scene_compositor::release() {
        state_.reset();
    }

}
