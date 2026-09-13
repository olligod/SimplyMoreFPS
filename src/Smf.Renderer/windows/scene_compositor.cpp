#define NOMINMAX
#include "scene_compositor.h"
#include "scene_shader_bytecode.h"
#include "../common/projection_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <wrl/client.h>

namespace smf_scene {

    using Microsoft::WRL::ComPtr;

    namespace {

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

        bool sampling_rows(const layer& source, const view& desired, const image& size, float* x, float* y) {
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
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> render_view;
            ComPtr<ID3D11ShaderResourceView> shader_view;
        };

        struct depth_target {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> shader_view;
            ComPtr<ID3D11UnorderedAccessView> output_view;
            uint32_t width = 0, height = 0;
        };

        struct depth_cache {
            std::array<depth_target, 4> levels;
            uint32_t level_count = 0;
            uint64_t texture = 0;
            uint64_t serial = 0;
            uint32_t width = 0, height = 0, flags = 0;
        };

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> background, layer, parallax, correction, additive;
        ComPtr<ID3D11ComputeShader> reduce_depth;
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11SamplerState> point_sampler, linear_sampler;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depth;
        std::array<target, 2> targets;
        std::array<depth_cache, 4> depth_bounds;
        uint32_t width = 0, height = 0, current = 0;
        bool output_valid = false;
        image_filter_executor* filter_executor = nullptr;

        HRESULT update_depth(uint32_t index, const image& metadata, ID3D11ShaderResourceView* source, uint32_t flags) {
            auto& cache = depth_bounds[index];
            if (!metadata.serial) return E_INVALIDARG;
            if (cache.texture == metadata.texture && cache.serial == metadata.serial && cache.width == metadata.width &&
                cache.height == metadata.height && cache.flags == flags) return S_OK;
            cache.serial = 0;
            if (cache.width != metadata.width || cache.height != metadata.height || !cache.level_count) {
                std::array<depth_target, 4> next;
                uint32_t level_count = 0;
                uint32_t level_width = metadata.width, level_height = metadata.height;
                do {
                    if (level_count == next.size()) return E_INVALIDARG;
                    level_width = (level_width + 15) / 16;
                    level_height = (level_height + 15) / 16;
                    depth_target item;
                    item.width = level_width;
                    item.height = level_height;
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = level_width;
                    desc.Height = level_height;
                    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = DXGI_FORMAT_R32_FLOAT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                    HRESULT result = device->CreateTexture2D(&desc, nullptr, &item.texture);
                    if (FAILED(result)) return result;
                    result = device->CreateShaderResourceView(item.texture.Get(), nullptr, &item.shader_view);
                    if (FAILED(result)) return result;
                    result = device->CreateUnorderedAccessView(item.texture.Get(), nullptr, &item.output_view);
                    if (FAILED(result)) return result;
                    next[level_count++] = std::move(item);
                } while (level_width > 1 || level_height > 1);
                cache.levels = std::move(next);
                cache.level_count = level_count;
                cache.width = metadata.width;
                cache.height = metadata.height;
            }
            constants data{};
            data.dimensions[0][0] = static_cast<float>(metadata.width);
            data.dimensions[0][1] = static_cast<float>(metadata.height);
            data.values[2] = 1;
            data.values[3] = (flags & negative_clip_depth) != 0 ? 1.f : 0.f;
            context->CSSetShader(reduce_depth.Get(), nullptr, 0);
            ID3D11Buffer* constant_buffer = buffer.Get();
            context->CSSetConstantBuffers(0, 1, &constant_buffer);
            for (uint32_t i = 0; i < cache.level_count; ++i) {
                auto& level = cache.levels[i];
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT result = context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(result)) return result;
                std::memcpy(mapped.pData, &data, sizeof(data));
                context->Unmap(buffer.Get(), 0);
                context->CSSetShaderResources(3, 1, &source);
                ID3D11UnorderedAccessView* output = level.output_view.Get();
                context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
                context->Dispatch(level.width, level.height, 1);
                ID3D11ShaderResourceView* no_source = nullptr;
                ID3D11UnorderedAccessView* no_output = nullptr;
                context->CSSetShaderResources(3, 1, &no_source);
                context->CSSetUnorderedAccessViews(0, 1, &no_output, nullptr);
                source = level.shader_view.Get();
                data.dimensions[0][0] = static_cast<float>(level.width);
                data.dimensions[0][1] = static_cast<float>(level.height);
                data.values[2] = 0;
            }
            context->CSSetShader(nullptr, nullptr, 0);
            cache.texture = metadata.texture;
            cache.serial = metadata.serial;
            cache.flags = flags;
            return S_OK;
        }

        void bind_pipeline() {
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(raster.Get());
            context->OMSetBlendState(blend.Get(), nullptr, UINT_MAX);
            context->OMSetDepthStencilState(depth.Get(), 0);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertex.Get(), nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer* constant_buffer = buffer.Get();
            context->PSSetConstantBuffers(0, 1, &constant_buffer);
        }

        HRESULT create_targets(uint32_t new_width, uint32_t new_height) {
            if (width == new_width && height == new_height) return S_OK;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = new_width;
            desc.Height = new_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            std::array<target, 2> next;
            for (auto& item : next) {
                HRESULT result = device->CreateTexture2D(&desc, nullptr, &item.texture);
                if (FAILED(result)) return result;
                result = device->CreateRenderTargetView(item.texture.Get(), nullptr, &item.render_view);
                if (FAILED(result)) return result;
                result = device->CreateShaderResourceView(item.texture.Get(), nullptr, &item.shader_view);
                if (FAILED(result)) return result;
            }
            targets = std::move(next);
            width = new_width;
            height = new_height;
            return S_OK;
        }

        HRESULT run(ID3D11PixelShader* shader, const constants& data,
            const std::array<ID3D11ShaderResourceView*, 64>& images, bool first = false) {
            bind_pipeline();
            const uint32_t next = first ? 0 : 1 - current;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT result = context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(result)) return result;
            std::memcpy(mapped.pData, &data, sizeof(data));
            context->Unmap(buffer.Get(), 0);

            ID3D11RenderTargetView* output = targets[next].render_view.Get();
            context->OMSetRenderTargets(1, &output, nullptr);
            context->PSSetShader(shader, nullptr, 0);
            context->PSSetShaderResources(0, static_cast<UINT>(images.size()), images.data());
            std::array<ID3D11SamplerState*, 16> samplers{};
            for (size_t i = 0; i < 11; ++i) {
                const uint32_t flags = data.image_flags[i / 4][i % 4];
                samplers[i] = (flags & linear_filter) != 0 ? linear_sampler.Get() : point_sampler.Get();
            }
            samplers[14] = linear_sampler.Get();
            samplers[15] = point_sampler.Get();
            context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
            context->Draw(3, 0);
            std::array<ID3D11ShaderResourceView*, 64> empty{};
            context->PSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
            context->OMSetRenderTargets(0, nullptr, nullptr);
            current = next;
            return S_OK;
        }
    };

    scene_compositor::scene_compositor() = default;
    scene_compositor::~scene_compositor() = default;

    HRESULT scene_compositor::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (state_) return E_UNEXPECTED;
        if (!device || !context || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return E_INVALIDARG;
        ComPtr<ID3D11Device> context_device;
        context->GetDevice(&context_device);
        if (context_device.Get() != device || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
        std::unique_ptr<state> next(new (std::nothrow) state);
        if (!next) return E_OUTOFMEMORY;
        next->device = device;
        next->context = context;
        const auto& vertex = smf_scene_shaders::vertex;
        HRESULT result = device->CreateVertexShader(vertex.data, vertex.size, nullptr, &next->vertex);
        if (FAILED(result)) return result;
        const smf_scene_shaders::program programs[] = {smf_scene_shaders::backgroundPixel, smf_scene_shaders::layerPixel,
            smf_scene_shaders::parallaxPixel, smf_scene_shaders::correctionPixel, smf_scene_shaders::additivePixel};
        ComPtr<ID3D11PixelShader>* shaders[] = {&next->background, &next->layer, &next->parallax, &next->correction, &next->additive};
        for (size_t i = 0; i < std::size(programs); ++i) {
            result = device->CreatePixelShader(programs[i].data, programs[i].size, nullptr,
                shaders[i]->GetAddressOf());
            if (FAILED(result)) return result;
        }
        const auto& reduce = smf_scene_shaders::reduceDepth;
        result = device->CreateComputeShader(reduce.data, reduce.size, nullptr, &next->reduce_depth);
        if (FAILED(result)) return result;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(constants);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device->CreateBuffer(&buffer, nullptr, &next->buffer);
        if (FAILED(result)) return result;
        D3D11_SAMPLER_DESC sampler{};
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        result = device->CreateSamplerState(&sampler, &next->point_sampler);
        if (FAILED(result)) return result;
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        result = device->CreateSamplerState(&sampler, &next->linear_sampler);
        if (FAILED(result)) return result;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        result = device->CreateRasterizerState(&raster, &next->raster);
        if (FAILED(result)) return result;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device->CreateBlendState(&blend, &next->blend);
        if (FAILED(result)) return result;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        result = device->CreateDepthStencilState(&depth, &next->depth);
        if (FAILED(result)) return result;
        state_ = std::move(next);
        return S_OK;
    }

    HRESULT scene_compositor::draw(const frame& source, const view& desired) {
        if (!state_) return E_UNEXPECTED;
        auto& state = *state_;
        state.output_valid = false;
        if (!source.scene || !source.images || !source.layers || !source.resources ||
            !desired.width || !desired.height || desired.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desired.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || !std::isfinite(desired.camera_x) ||
            !std::isfinite(desired.camera_z)) return E_INVALIDARG;
        const auto& scene = *source.scene;
        if (scene.size != sizeof(description) || scene.version != version ||
            scene.image_count > maximum_images || scene.layer_count > maximum_layers ||
            scene.effect_count > maximum_effects || (scene.effect_count && !source.effects)) return E_INVALIDARG;
        const auto valid_image = [&](uint32_t index) {
            return index < scene.image_count && source.resources[index] &&
                source.images[index].width && source.images[index].height &&
                source.images[index].width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                source.images[index].height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
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
                if (!state.filter_executor || !state.filter_executor->supports(effect.program, effect.pass)) return E_NOTIMPL;
                continue;
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
        const double camera_x = std::clamp(desired.camera_x, world.min_x, world.max_x);
        const double camera_z = std::clamp(desired.camera_z, world.min_z, world.max_z);
        data.camera_delta[0] = static_cast<float>((camera_x - std::clamp(world.source_x, world.min_x, world.max_x)) * world.camera_x_per_cell);
        data.camera_delta[1] = static_cast<float>((camera_z - std::clamp(world.source_z, world.min_z, world.max_z)) * world.camera_y_per_cell);
        data.values[2] = data.camera_delta[0] == 0 && data.camera_delta[1] == 0 ? 1.f : 0.f;
        for (float value : data.camera_delta) if (!std::isfinite(value)) return E_INVALIDARG;
        data.image_flags[0][1] = source.images[world.live_color].flags;
        data.image_flags[0][2] = source.images[world.sky].flags;
        data.glow_counts[0] = world.live_glow_count;
        data.glow_counts[1] = world.cached_glow_count;
        std::memcpy(data.live_glows, world.live_glows, sizeof(data.live_glows));
        std::memcpy(data.cached_glows, world.cached_glows, sizeof(data.cached_glows));

        HRESULT result = state.create_targets(desired.width, desired.height);
        if (FAILED(result)) return result;
        std::array<ID3D11ShaderResourceView*, 64> images{};
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
            images[11 + i] = bounds.levels[bounds.level_count - 1].shader_view.Get();
        }
        result = state.run(state.background.Get(), data, images, true);
        if (FAILED(result)) return result;

        constants parallax_data{};
        std::array<ID3D11ShaderResourceView*, 64> parallax_images{};
        parallax_images[0] = state.targets[state.current].shader_view.Get();
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
            result = state.run(state.parallax.Get(), parallax_data, parallax_images);
            if (FAILED(result)) return result;
        }

        const auto draw_layer = [&](const smf_scene::layer* foreground, const smf_scene::layer& cached) {
            constants layer_data{};
            std::array<ID3D11ShaderResourceView*, 64> bindings{};
            bindings[0] = state.targets[state.current].shader_view.Get();
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
            return state.run(state.layer.Get(), layer_data, bindings);
        };
        result = draw_layer(live, *cache);
        if (FAILED(result)) return result;

        for (uint32_t i = 0; i < scene.effect_count; ++i) {
            const auto& effect = source.effects[i];
            if (effect.kind == image_filter) {
                const uint32_t next = 1 - state.current;
                state.bind_pipeline();
                result = state.filter_executor->execute(state.context.Get(), source, effect,
                    state.targets[state.current].shader_view.Get(), state.targets[next].render_view.Get(),
                    state.width, state.height);
                std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> empty{};
                state.context->PSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
                state.context->VSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
                state.context->OMSetRenderTargets(0, nullptr, nullptr);
                if (FAILED(result)) return result;
                state.current = next;
                continue;
            }
            constants effect_data{};
            effect_data.values[0] = effect.parameters[0];
            effect_data.image_flags[0][1] = source.images[effect.first_image].flags;
            std::array<ID3D11ShaderResourceView*, 64> bindings{};
            bindings[0] = state.targets[state.current].shader_view.Get();
            bindings[1] = source.resources[effect.first_image];
            ID3D11PixelShader* shader = state.correction.Get();
            if (effect.kind == additive_image) {
                shader = state.additive.Get();
                bindings[4] = source.resources[effect.second_image];
                effect_data.image_flags[1][0] = source.images[effect.second_image].flags;
                if (!sampling_rows(*live, desired, source.images[effect.first_image], effect_data.rows[0], effect_data.rows[1])) return E_INVALIDARG;
                if (!sampling_rows(*cache, desired, source.images[effect.second_image], effect_data.rows[2], effect_data.rows[3])) return E_INVALIDARG;
            }
            result = state.run(shader, effect_data, bindings);
            if (FAILED(result)) return result;
        }
        result = state.device->GetDeviceRemovedReason();
        if (FAILED(result)) return result;
        state.output_valid = true;
        return S_OK;
    }

    ID3D11Texture2D* scene_compositor::texture() const {
        return state_ && state_->output_valid ? state_->targets[state_->current].texture.Get() : nullptr;
    }

    void scene_compositor::set_filter_executor(image_filter_executor* executor) {
        if (state_) state_->filter_executor = executor;
    }

    ID3D11ShaderResourceView* scene_compositor::shader_resource() const {
        return state_ && state_->output_valid ? state_->targets[state_->current].shader_view.Get() : nullptr;
    }

    void scene_compositor::release() {
        state_.reset();
    }

}
