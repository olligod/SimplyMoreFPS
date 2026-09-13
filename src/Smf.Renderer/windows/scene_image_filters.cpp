#include "scene_image_filters.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace smf_scene {

    using Microsoft::WRL::ComPtr;

    namespace {
        struct buffer_description {
            uint32_t slot = 0, size = 0;
            std::vector<filter_parameter> parameters;
        };

        struct stage_description {
            std::vector<uint8_t> bytecode;
            std::vector<filter_texture> textures;
            std::vector<buffer_description> buffers;
        };

        struct pass_description {
            stage_description vertex, fragment;
        };

        struct program_description {
            std::vector<pass_description> passes;
        };

        std::mutex registry_mutex;
        std::map<uint64_t, std::shared_ptr<const program_description>> registry;
        uint64_t next_program = 1;

        std::shared_ptr<const program_description> find_program(uint64_t handle) {
            std::lock_guard<std::mutex> lock(registry_mutex);
            auto found = registry.find(handle);
            return found == registry.end() ? nullptr : found->second;
        }

        bool read_stage(const filter_stage& source, stage_description& target) {
            if (!source.bytecode || source.bytecode_size < 32 || source.bytecode_size > 1024 * 1024 ||
                source.texture_count > 4 || source.buffer_count > 8 || source.reserved ||
                (source.texture_count && !source.textures) || (source.buffer_count && !source.buffers)) return false;
            const auto* bytes = reinterpret_cast<const uint8_t*>(source.bytecode);
            if (std::memcmp(bytes, "DXBC", 4)) return false;
            target.bytecode.assign(bytes, bytes + source.bytecode_size);

            uint32_t textures_seen = 0;
            const auto* textures = reinterpret_cast<const filter_texture*>(source.textures);
            for (uint32_t i = 0; i < source.texture_count; ++i) {
                const auto& item = textures[i];
                if (item.role < main_texture || item.role > blend_texture || item.texture_slot >= 16 ||
                    item.sampler_slot >= 16 || item.reserved || (textures_seen & (1u << item.texture_slot))) return false;
                textures_seen |= 1u << item.texture_slot;
                target.textures.push_back(item);
            }

            uint32_t buffers_seen = 0;
            const auto* buffers = reinterpret_cast<const filter_buffer*>(source.buffers);
            for (uint32_t i = 0; i < source.buffer_count; ++i) {
                const auto& item = buffers[i];
                if (item.slot >= 14 || !item.size || item.size > 4096 || item.size % 16 || item.parameter_count > 32 ||
                    (item.parameter_count && !item.parameters) || item.reserved || (buffers_seen & (1u << item.slot))) return false;
                buffers_seen |= 1u << item.slot;
                buffer_description buffer;
                buffer.slot = item.slot;
                buffer.size = item.size;
                const auto* parameters = reinterpret_cast<const filter_parameter*>(item.parameters);
                std::array<bool, 1024> occupied{};
                for (uint32_t j = 0; j < item.parameter_count; ++j) {
                    const auto& parameter = parameters[j];
                    const bool matrix = parameter.role == object_to_world || parameter.role == matrix_vp;
                    const uint32_t columns = parameter.role <= texel_size || matrix ? 4 : 1;
                    const uint32_t rows = matrix ? 4 : 1;
                    if (parameter.role < main_texel_size || parameter.role > matrix_vp || parameter.rows != rows ||
                        parameter.columns != columns || parameter.offset % 4 || parameter.offset > item.size ||
                        rows * columns * 4 > item.size - parameter.offset) return false;
                    for (uint32_t k = 0; k < rows * columns; ++k) {
                        const auto index = parameter.offset / 4 + k;
                        if (occupied[index]) return false;
                        occupied[index] = true;
                    }
                    buffer.parameters.push_back(parameter);
                }
                target.buffers.push_back(std::move(buffer));
            }
            return true;
        }

        struct gpu_stage {
            std::vector<ComPtr<ID3D11Buffer>> buffers;
        };

        struct gpu_pass {
            ComPtr<ID3D11VertexShader> vertex;
            ComPtr<ID3D11PixelShader> fragment;
            ComPtr<ID3D11InputLayout> layout;
            gpu_stage vertex_data, fragment_data;
        };

        struct gpu_program {
            std::shared_ptr<const program_description> description;
            std::vector<gpu_pass> passes;
        };

        struct target {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> source;
            ComPtr<ID3D11RenderTargetView> output;
            uint32_t width = 0, height = 0;

            HRESULT create(ID3D11Device* device, uint32_t w, uint32_t h) {
                if (texture && width == w && height == h) return S_OK;
                target replacement;
                D3D11_TEXTURE2D_DESC description{};
                description.Width = w;
                description.Height = h;
                description.MipLevels = description.ArraySize = 1;
                description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                description.SampleDesc.Count = 1;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                HRESULT result = device->CreateTexture2D(&description, nullptr, &replacement.texture);
                if (SUCCEEDED(result)) result = device->CreateShaderResourceView(replacement.texture.Get(), nullptr, &replacement.source);
                if (SUCCEEDED(result)) result = device->CreateRenderTargetView(replacement.texture.Get(), nullptr, &replacement.output);
                if (FAILED(result)) return result;
                replacement.width = w;
                replacement.height = h;
                *this = std::move(replacement);
                return S_OK;
            }
        };

        struct lookup_image {
            target image;
            uint64_t texture = 0, serial = 0;
            uint32_t flags = 0;
        };

        constexpr const char* copy_shader = R"(
Texture2D<float4> source : register(t0);
cbuffer Parameters : register(b0) { uint4 values; };
struct Vertex { float4 position : SV_Position; };
Vertex vert(uint id : SV_VertexID) {
    Vertex result;
    result.position = float4(id == 2 ? 3 : -1, id == 1 ? -3 : 1, 0, 1);
    return result;
}
float4 frag(Vertex input) : SV_Target {
    int2 pixel = int2(input.position.xy);
    if (values.z != 0) pixel.y = int(values.y) - 1 - pixel.y;
    return source.Load(int3(pixel, 0));
}
)";
    }

    struct scene_image_filters::state {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11Buffer> vertices, copy_constants;
        ComPtr<ID3D11VertexShader> copy_vertex;
        ComPtr<ID3D11PixelShader> copy_fragment;
        ComPtr<ID3D11SamplerState> point, linear;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11BlendState> blend;
        std::map<uint64_t, gpu_program> programs;
        std::array<lookup_image, 2> lookups;
        target edges, weights;
        HRESULT initialization = E_PENDING;

        HRESULT initialize(ID3D11DeviceContext* context) {
            if (device) {
                ComPtr<ID3D11Device> actual;
                context->GetDevice(&actual);
                return actual == device ? initialization : E_INVALIDARG;
            }
            context->GetDevice(&device);
            if (!device) return E_INVALIDARG;
            struct vertex { float position[4], uv[2]; };
            const vertex data[] = {
                {{-1, 1, 0, 1}, {0, 0}}, {{-1, -3, 0, 1}, {0, 2}}, {{3, 1, 0, 1}, {2, 0}}
            };
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(data);
            buffer.Usage = D3D11_USAGE_IMMUTABLE;
            buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = data;
            HRESULT result = device->CreateBuffer(&buffer, &initial, &vertices);
            buffer = {};
            buffer.ByteWidth = 16;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (SUCCEEDED(result)) result = device->CreateBuffer(&buffer, nullptr, &copy_constants);
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &point);
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &linear);
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = true;
            if (SUCCEEDED(result)) result = device->CreateRasterizerState(&raster, &rasterizer);
            D3D11_DEPTH_STENCIL_DESC depth_description{};
            if (SUCCEEDED(result)) result = device->CreateDepthStencilState(&depth_description, &depth);
            D3D11_BLEND_DESC blend_description{};
            blend_description.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (SUCCEEDED(result)) result = device->CreateBlendState(&blend_description, &blend);
            ComPtr<ID3DBlob> vertex_code, pixel_code, errors;
            if (SUCCEEDED(result)) result = D3DCompile(copy_shader, std::strlen(copy_shader), nullptr, nullptr, nullptr,
                "vert", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex_code, &errors);
            if (SUCCEEDED(result)) result = D3DCompile(copy_shader, std::strlen(copy_shader), nullptr, nullptr, nullptr,
                "frag", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel_code, &errors);
            if (SUCCEEDED(result)) result = device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(),
                nullptr, &copy_vertex);
            if (SUCCEEDED(result)) result = device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(),
                nullptr, &copy_fragment);
            initialization = result;
            return result;
        }

        HRESULT create_buffers(const stage_description& source, gpu_stage& output) {
            for (const auto& item : source.buffers) {
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = item.size;
                description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                ComPtr<ID3D11Buffer> buffer;
                HRESULT result = device->CreateBuffer(&description, nullptr, &buffer);
                if (FAILED(result)) return result;
                output.buffers.push_back(std::move(buffer));
            }
            return S_OK;
        }

        HRESULT create_program(uint64_t handle, gpu_program*& output) {
            auto found = programs.find(handle);
            if (found != programs.end()) {
                output = &found->second;
                return S_OK;
            }
            gpu_program program;
            program.description = find_program(handle);
            if (!program.description) return E_INVALIDARG;
            for (const auto& pass : program.description->passes) {
                gpu_pass result;
                HRESULT status = device->CreateVertexShader(pass.vertex.bytecode.data(), pass.vertex.bytecode.size(), nullptr, &result.vertex);
                if (SUCCEEDED(status)) status = device->CreatePixelShader(pass.fragment.bytecode.data(), pass.fragment.bytecode.size(), nullptr, &result.fragment);
                const D3D11_INPUT_ELEMENT_DESC layout[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
                };
                if (SUCCEEDED(status)) status = device->CreateInputLayout(layout, 2, pass.vertex.bytecode.data(), pass.vertex.bytecode.size(), &result.layout);
                if (SUCCEEDED(status)) status = create_buffers(pass.vertex, result.vertex_data);
                if (SUCCEEDED(status)) status = create_buffers(pass.fragment, result.fragment_data);
                if (FAILED(status)) return status;
                program.passes.push_back(std::move(result));
            }
            output = &programs.emplace(handle, std::move(program)).first->second;
            return S_OK;
        }

        static void unbind(ID3D11DeviceContext* context) {
            ID3D11ShaderResourceView* empty[16]{};
            context->VSSetShaderResources(0, 16, empty);
            context->PSSetShaderResources(0, 16, empty);
            context->OMSetRenderTargets(0, nullptr, nullptr);
        }

        void output_state(ID3D11DeviceContext* context, ID3D11RenderTargetView* output, uint32_t width, uint32_t height) {
            unbind(context);
            D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizer.Get());
            context->OMSetDepthStencilState(depth.Get(), 0);
            context->OMSetBlendState(blend.Get(), nullptr, UINT32_MAX);
            context->OMSetRenderTargets(1, &output, nullptr);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        HRESULT lookup(ID3D11DeviceContext* context, const frame& source, uint32_t index,
            size_t slot, ID3D11ShaderResourceView*& result) {
            result = nullptr;
            if (index == no_image) return S_OK;
            if (index >= source.scene->image_count || slot >= lookups.size()) return E_INVALIDARG;
            const auto& metadata = source.images[index];
            auto& saved = lookups[slot];
            if (saved.texture != metadata.texture || saved.serial != metadata.serial ||
                saved.flags != metadata.flags || saved.image.width != metadata.width ||
                saved.image.height != metadata.height) {
                HRESULT status = saved.image.create(device.Get(), metadata.width, metadata.height);
                if (FAILED(status)) return status;
                output_state(context, saved.image.output.Get(), metadata.width, metadata.height);
                context->IASetInputLayout(nullptr);
                context->VSSetShader(copy_vertex.Get(), nullptr, 0);
                context->PSSetShader(copy_fragment.Get(), nullptr, 0);
                const uint32_t values[4] = {metadata.width, metadata.height, metadata.flags & flip_y, 0};
                context->UpdateSubresource(copy_constants.Get(), 0, nullptr, values, 0, 0);
                ID3D11Buffer* buffer = copy_constants.Get();
                context->PSSetConstantBuffers(0, 1, &buffer);
                context->PSSetShaderResources(0, 1, &source.resources[index]);
                context->Draw(3, 0);
                unbind(context);
                saved.texture = metadata.texture;
                saved.serial = metadata.serial;
                saved.flags = metadata.flags;
            }
            result = saved.image.source.Get();
            return S_OK;
        }

        void bind_stage(ID3D11DeviceContext* context, const stage_description& description, const gpu_stage& gpu,
            const effect& operation, uint32_t width, uint32_t height, ID3D11ShaderResourceView* const* resources,
            bool vertex, bool linear_main) {
            for (size_t i = 0; i < description.buffers.size(); ++i) {
                const auto& buffer = description.buffers[i];
                std::array<float, 1024> values{};
                for (const auto& parameter : buffer.parameters) {
                    float* value = values.data() + parameter.offset / 4;
                    switch (parameter.role) {
                    case main_texel_size:
                    case texel_size:
                        value[0] = 1.f / width;
                        value[1] = 1.f / height;
                        value[2] = static_cast<float>(width);
                        value[3] = static_cast<float>(height);
                        break;
                    case object_to_world:
                    case matrix_vp:
                        value[0] = value[5] = value[10] = value[15] = 1;
                        break;
                    default:
                        value[0] = operation.parameters[parameter.role - subpixel_blending];
                        break;
                    }
                }
                ID3D11Buffer* native = gpu.buffers[i].Get();
                context->UpdateSubresource(native, 0, nullptr, values.data(), 0, 0);
                if (vertex) context->VSSetConstantBuffers(buffer.slot, 1, &native);
                else context->PSSetConstantBuffers(buffer.slot, 1, &native);
            }

            for (const auto& texture : description.textures) {
                ID3D11ShaderResourceView* image = resources[texture.role];
                bool smooth = texture.role == area_texture || texture.role == blend_texture ||
                    (texture.role == main_texture && linear_main);
                ID3D11SamplerState* sampler = smooth ? linear.Get() : point.Get();
                if (vertex) {
                    context->VSSetShaderResources(texture.texture_slot, 1, &image);
                    context->VSSetSamplers(texture.sampler_slot, 1, &sampler);
                } else {
                    context->PSSetShaderResources(texture.texture_slot, 1, &image);
                    context->PSSetSamplers(texture.sampler_slot, 1, &sampler);
                }
            }
        }
    };

    scene_image_filters::scene_image_filters() : state_(new state) {}
    scene_image_filters::~scene_image_filters() = default;

    bool scene_image_filters::supports(uint64_t program, uint32_t pass) const {
        return pass == 0 && find_program(program) != nullptr;
    }

    HRESULT scene_image_filters::execute(ID3D11DeviceContext* context, const frame& source, const effect& operation,
        ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, uint32_t width, uint32_t height) {
        try {
            return execute_frame(context, source, operation, input, output, width, height);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

    HRESULT scene_image_filters::execute_frame(ID3D11DeviceContext* context, const frame& source, const effect& operation,
        ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, uint32_t width, uint32_t height) {
        if (!context || !input || !output || !source.scene || !width || !height || operation.pass) return E_INVALIDARG;
        auto& state = *state_;
        HRESULT result = state.initialize(context);
        gpu_program* program = nullptr;
        if (SUCCEEDED(result)) result = state.create_program(operation.program, program);
        if (FAILED(result)) return result;
        const bool smaa = program->passes.size() == 3;
        if (smaa && (operation.first_image == no_image || operation.second_image == no_image)) return E_INVALIDARG;
        ID3D11ShaderResourceView* resources[5]{};
        if (SUCCEEDED(result)) result = state.lookup(context, source, operation.first_image, 0, resources[area_texture]);
        if (SUCCEEDED(result)) result = state.lookup(context, source, operation.second_image, 1, resources[search_texture]);
        if (smaa && SUCCEEDED(result)) result = state.edges.create(state.device.Get(), width, height);
        if (smaa && SUCCEEDED(result)) result = state.weights.create(state.device.Get(), width, height);
        if (FAILED(result)) return result;

        for (size_t i = 0; i < program->passes.size(); ++i) {
            const auto& gpu = program->passes[i];
            const auto& description = program->description->passes[i];
            ID3D11RenderTargetView* destination = !smaa || i == 2 ? output :
                (i == 0 ? state.edges.output.Get() : state.weights.output.Get());
            state.output_state(context, destination, width, height);
            if (smaa && i == 0) {
                const float clear[4]{};
                context->ClearRenderTargetView(destination, clear);
            }
            resources[main_texture] = smaa && i == 1 ? state.edges.source.Get() : input;
            resources[blend_texture] = smaa && i == 2 ? state.weights.source.Get() : nullptr;
            context->IASetInputLayout(gpu.layout.Get());
            ID3D11Buffer* vertices = state.vertices.Get();
            const UINT stride = 24, offset = 0;
            context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
            context->VSSetShader(gpu.vertex.Get(), nullptr, 0);
            context->PSSetShader(gpu.fragment.Get(), nullptr, 0);
            const bool linear_main = (smaa && i == 1) || operation.parameters[15] != 0;
            state.bind_stage(context, description.vertex, gpu.vertex_data, operation, width, height, resources, true, linear_main);
            state.bind_stage(context, description.fragment, gpu.fragment_data, operation, width, height, resources, false, linear_main);
            context->Draw(3, 0);
        }
        state::unbind(context);
        return S_OK;
    }
}

int __cdecl smf_scene_register_filter(const smf_scene::filter_definition* definition, uint64_t* handle) {
    using namespace smf_scene;
    if (!definition || !handle) return E_INVALIDARG;
    *handle = 0;
    if (definition->size != sizeof(filter_definition) || definition->version != 1 || definition->reserved ||
        (definition->pass_count != 1 && definition->pass_count != 3) || !definition->passes) return E_INVALIDARG;
    try {
        auto program = std::make_shared<program_description>();
        const auto* passes = reinterpret_cast<const filter_pass*>(definition->passes);
        for (uint32_t i = 0; i < definition->pass_count; ++i) {
            pass_description pass;
            if (!read_stage(passes[i].vertex, pass.vertex) || !read_stage(passes[i].fragment, pass.fragment)) return E_INVALIDARG;
            program->passes.push_back(std::move(pass));
        }
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (registry.size() >= 64) return E_OUTOFMEMORY;
        const uint64_t id = next_program++;
        registry.emplace(id, std::move(program));
        *handle = id;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
