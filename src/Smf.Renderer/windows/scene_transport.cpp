#include "scene_transport.h"
#include "scene_image_filters.h"
#include "scene_memory.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace session {
    namespace {

        scene_memory_budget copy_budget{uint64_t{2} * 1024 * 1024 * 1024};

        struct copy_reservation {
            uint64_t bytes = 0;
            ~copy_reservation() { copy_budget.release(bytes); }
        };

        DXGI_FORMAT image_format(DXGI_FORMAT value, bool depth) {
            if (depth) return value == DXGI_FORMAT_R32_FLOAT || value == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
            if (value == DXGI_FORMAT_R16G16B16A16_FLOAT || value == DXGI_FORMAT_R16G16B16A16_TYPELESS) return DXGI_FORMAT_R16G16B16A16_FLOAT;
            return compatible_format(value);
        }

        HRESULT query_ready(ID3D11DeviceContext* context, ID3D11Query* query) {
            BOOL completed = FALSE;
            const HRESULT hr = context->GetData(query, &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            return hr == S_OK && !completed ? S_FALSE : hr;
        }

        HRESULT make_query(ID3D11Device* device, ComPtr<ID3D11Query>& query) {
            const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
            return device->CreateQuery(&description, &query);
        }

        bool same_image(const smf_scene::image& a, const smf_scene::image& b) {
            return a.texture == b.texture && a.serial == b.serial && a.width == b.width &&
                a.height == b.height && a.flags == b.flags;
        }

        void release_bundle(scene_bundle& bundle) {
            bundle.images = {};
            bundle.phase.store(scene_phase::free, std::memory_order_release);
        }

        constexpr char transfer_shader[] = R"(
Texture2D<float4> sourceImage : register(t0);
cbuffer Transfer : register(b0) { float2 targetOffset; float2 padding; };
float4 vertex(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 pixel(float4 position : SV_Position) : SV_Target {
    return float4(sourceImage.Load(int3(int2(position.xy - targetOffset), 0)).rgb, 1);
}
)";

    }

    HRESULT scene_channel::initialize(ID3D11Device* source_device, ID3D11DeviceContext* source_context) {
        if (!source_device || !source_context) return E_POINTER;
        ComPtr<ID3D11Device> owner;
        source_context->GetDevice(&owner);
        if (owner.Get() != source_device) return E_INVALIDARG;
        ComPtr<IDXGIDevice> dxgi;
        HRESULT hr = source_device->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
        if (FAILED(hr)) return hr;
        device = source_device;
        context = source_context;
        try {
            pool.reserve(capacity * (smf_scene::maximum_images + 2));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    scene_image::~scene_image() {
        view.Reset();
        worker.Reset();
        source.Reset();
        original.Reset();
        copy_budget.release(allocation_bytes);
    }

    HRESULT scene_channel::image_cost(const smf_scene::image& description, DXGI_FORMAT& format, uint64_t& bytes) const {
        auto* input = reinterpret_cast<ID3D11Texture2D*>(description.texture);
        if (!input) return E_POINTER;
        D3D11_TEXTURE2D_DESC source{};
        input->GetDesc(&source);
        ComPtr<ID3D11Device> owner;
        input->GetDevice(&owner);
        format = image_format(source.Format, (description.flags & smf_scene::depth_image) != 0);
        if (owner.Get() != device.Get() || source.Width != description.width || source.Height != description.height ||
            source.MipLevels != 1 || source.ArraySize != 1 || source.SampleDesc.Count != 1 ||
            source.SampleDesc.Quality || format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        bytes = uint64_t(source.Width) * source.Height * (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
        return S_OK;
    }

    HRESULT scene_channel::copy_cost(const smf_scene::snapshot& snapshot, const session_frame& frame,
        const model& camera, uint64_t& required) const {
        constexpr size_t limit = capacity * (smf_scene::maximum_images + 2);
        struct planned_image {
            smf_scene::image description{};
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            bool used = false;
        };
        std::array<planned_image, limit> planned{};
        size_t count = pool.size();
        for (size_t i = 0; i < count; ++i)
            planned[i] = {pool[i]->description, pool[i]->format, pool[i].use_count() != 1};
        std::array<smf_scene::image, smf_scene::maximum_images + 2> inputs{};
        std::copy_n(snapshot.images.begin(), snapshot.frame.image_count, inputs.begin());
        inputs[snapshot.frame.image_count] = {frame.world_texture, frame.source_frame, camera.width, camera.height, 0, 0};
        inputs[snapshot.frame.image_count + 1] = {frame.hud_texture, frame.source_frame, camera.width, camera.height, 0, 0};
        uint64_t intrinsic = 0;
        required = 0;
        for (uint32_t i = 0; i < snapshot.frame.image_count + 2; ++i) {
            const auto& input = inputs[i];
            DXGI_FORMAT format{};
            uint64_t bytes = 0;
            const HRESULT hr = image_cost(input, format, bytes);
            if (FAILED(hr)) return hr;
            const auto duplicate = std::find_if(inputs.begin(), inputs.begin() + i,
                [&](const auto& previous) { return same_image(previous, input); });
            if (duplicate == inputs.begin() + i) {
                // A complete replacement must fit beside the retained current frame.
                if (bytes > copy_budget.limit() / 2 - intrinsic) return E_OUTOFMEMORY;
                intrinsic += bytes;
            }
            auto exact = std::find_if(planned.begin(), planned.begin() + count,
                [&](const auto& item) { return same_image(item.description, input); });
            if (exact != planned.begin() + count) {
                exact->used = true;
                continue;
            }
            auto reusable = std::find_if(planned.begin(), planned.begin() + count, [&](const auto& item) {
                return !item.used && item.format == format && item.description.width == input.width &&
                    item.description.height == input.height;
            });
            if (reusable == planned.begin() + count) {
                if (count == limit) {
                    reusable = std::find_if(planned.begin(), planned.end(), [](const auto& item) { return !item.used; });
                    if (reusable == planned.end()) return E_OUTOFMEMORY;
                    std::move(reusable + 1, planned.end(), reusable);
                    --count;
                }
                reusable = planned.begin() + count++;
                required += bytes;
            }
            *reusable = {input, format, true};
        }
        return S_OK;
    }

    HRESULT scene_channel::copy_image(const smf_scene::image& description, std::shared_ptr<scene_image>& output,
        uint64_t& reserved) {
        DXGI_FORMAT format{};
        uint64_t bytes = 0;
        const HRESULT validation = image_cost(description, format, bytes);
        if (FAILED(validation)) return validation;
        auto* input = reinterpret_cast<ID3D11Texture2D*>(description.texture);

        for (const auto& image : pool) {
            if (same_image(image->description, description)) {
                output = image;
                return S_OK;
            }
        }

        std::shared_ptr<scene_image> destination;
        for (const auto& image : pool) {
            if (image.use_count() == 1 && image->format == format &&
                image->description.width == description.width && image->description.height == description.height) {
                destination = image;
                break;
            }
        }

        if (!destination) {
            if (bytes > reserved) return E_UNEXPECTED;
            if (pool.size() == capacity * (smf_scene::maximum_images + 2)) {
                const auto unused = std::find_if(pool.begin(), pool.end(), [](const auto& image) { return image.use_count() == 1; });
                if (unused == pool.end()) return E_OUTOFMEMORY;
                pool.erase(unused);
            }
            try {
                destination = std::make_shared<scene_image>();
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            }
            D3D11_TEXTURE2D_DESC copy{};
            copy.Width = description.width;
            copy.Height = description.height;
            copy.MipLevels = copy.ArraySize = copy.SampleDesc.Count = 1;
            copy.Format = format;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            copy.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            HRESULT hr = device->CreateTexture2D(&copy, nullptr, &destination->source);
            ComPtr<IDXGIResource> resource;
            if (SUCCEEDED(hr)) hr = destination->source.As(&resource);
            if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&destination->handle);
            if (FAILED(hr)) return hr;
            destination->format = format;
            destination->allocation_bytes = bytes;
            reserved -= bytes;
            pool.push_back(destination);
        }

        destination->description = description;
        destination->original = input;
        context->CopyResource(destination->source.Get(), input);
        output = std::move(destination);
        return S_OK;
    }

    HRESULT scene_channel::submit(const smf_scene::snapshot& snapshot, const session_frame& frame, const model& camera) {
        if (retiring.load()) return E_ABORT;
        if (!snapshot.frame.image_count || snapshot.frame.image_count > smf_scene::maximum_images) return E_INVALIDARG;
        scene_bundle* selected = nullptr;
        for (auto& bundle : bundles) {
            if (bundle.phase.load(std::memory_order_acquire) == scene_phase::free) {
                selected = &bundle;
                break;
            }
        }
        if (!selected) return S_FALSE;

        copy_reservation reservation;
        uint64_t required = 0;
        HRESULT hr = copy_cost(snapshot, frame, camera, required);
        if (FAILED(hr)) return hr;
        if (!copy_budget.reserve(required)) {
            trim();
            hr = copy_cost(snapshot, frame, camera, required);
            if (FAILED(hr)) return hr;
            if (!copy_budget.reserve(required)) return S_FALSE;
        }
        reservation.bytes = required;

        auto& bundle = *selected;
        bundle.images = {};
        bundle.snapshot = snapshot;
        bundle.frame = frame;
        bundle.frame.scene_description = 0;
        bundle.camera = camera;
        if (!bundle.source_completion) {
            hr = make_query(device.Get(), bundle.source_completion);
            if (FAILED(hr)) return hr;
        }

        for (uint32_t i = 0; i < snapshot.frame.image_count && SUCCEEDED(hr); ++i)
            hr = copy_image(snapshot.images[i], bundle.images[i], reservation.bytes);
        const smf_scene::image world_image{frame.world_texture, frame.source_frame, camera.width, camera.height, 0, 0};
        const smf_scene::image hud_image{frame.hud_texture, frame.source_frame, camera.width, camera.height, 0, 0};
        if (SUCCEEDED(hr)) hr = copy_image(world_image, bundle.images[snapshot.frame.image_count], reservation.bytes);
        if (SUCCEEDED(hr)) hr = copy_image(hud_image, bundle.images[snapshot.frame.image_count + 1], reservation.bytes);

        // Even a rejected partial copy retains its inputs until the source fence completes.
        context->End(bundle.source_completion.Get());
        context->Flush();
        if (FAILED(hr)) bundle.frame.source_frame = 0;
        bundle.phase.store(scene_phase::copying, std::memory_order_release);
        trim();
        return hr;
    }

    HRESULT scene_channel::poll(bool discard) {
        for (auto& bundle : bundles) {
            if (bundle.phase.load(std::memory_order_acquire) != scene_phase::copying) continue;
            const HRESULT hr = query_ready(context.Get(), bundle.source_completion.Get());
            if (hr == S_FALSE) continue;
            if (FAILED(hr)) return hr;
            if (discard || retiring.load() || !bundle.frame.source_frame) release_bundle(bundle);
            else bundle.phase.store(scene_phase::ready, std::memory_order_release);
        }
        if (discard || retiring.load()) {
            discard_ready();
            trim();
        }
        return S_OK;
    }

    void scene_channel::discard_ready() {
        for (auto& bundle : bundles) {
            auto expected = scene_phase::ready;
            if (bundle.phase.compare_exchange_strong(expected, scene_phase::reading)) release_bundle(bundle);
        }
    }

    void scene_channel::trim() {
        pool.erase(std::remove_if(pool.begin(), pool.end(), [](const auto& image) {
            return image.use_count() == 1;
        }), pool.end());
    }

    scene_bundle* scene_channel::acquire() {
        scene_bundle* newest = nullptr;
        for (auto& bundle : bundles) {
            auto expected = scene_phase::ready;
            if (!bundle.phase.compare_exchange_strong(expected, scene_phase::reading)) continue;
            if (!newest || bundle.frame.source_frame > newest->frame.source_frame) {
                if (newest) release_bundle(*newest);
                newest = &bundle;
            } else {
                release_bundle(bundle);
            }
        }
        if (newest && retiring.load()) {
            release_bundle(*newest);
            return nullptr;
        }
        return newest;
    }

    bool scene_channel::idle() const {
        for (const auto& bundle : bundles) {
            if (bundle.phase.load(std::memory_order_acquire) != scene_phase::free) return false;
        }
        return true;
    }

    struct scene_worker::implementation {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDCompositionDevice> composition;
        ComPtr<IDCompositionSurface> background_surface;
        ComPtr<IDCompositionSurface> world_surface;
        ComPtr<IDCompositionSurface> hud_surface;
        ComPtr<IDCompositionVisual> background;
        ComPtr<IDCompositionVisual> world;
        ComPtr<IDCompositionVisual> hud;
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11Buffer> transfer;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11Query> presentation_completion;
        smf_scene::scene_compositor compositor;
        smf_scene::scene_image_filters filters;
        scene_bundle* current = nullptr;
        std::array<scene_bundle*, scene_channel::capacity> pending{};
        smf_scene::view desired{};
        smf_scene::view displayed{};
        D2D_MATRIX_3X2_F world_transform{1, 0, 0, 1, 0, 0};
        uint64_t displayed_frame = 0;
        uint64_t initial_completion = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT world_format = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT hud_format = DXGI_FORMAT_UNKNOWN;
        bool dirty = false;
        bool ready = false;
        bool reused = false;
        bool presentation_pending = false;
        int64_t frequency = 0;
        int64_t interval = 0;
        int64_t next_frame = 0;
        int64_t next_monitor_check = 0;

        void update_cadence(int64_t timestamp) {
            if (timestamp < next_monitor_check) return;
            next_monitor_check = timestamp + frequency;
            MONITORINFOEXW monitor{};
            monitor.cbSize = sizeof(monitor);
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            uint32_t refresh = 60;
            if (GetMonitorInfoW(MonitorFromWindow(session::state().window, MONITOR_DEFAULTTOPRIMARY), &monitor) &&
                EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency >= 24 && mode.dmDisplayFrequency <= 1000) refresh = mode.dmDisplayFrequency;
            interval = frequency / refresh;
        }

        HRESULT initialize(scene_channel& channel) {
            D3D_FEATURE_LEVEL level{};
            const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
            HRESULT hr = D3D11CreateDevice(channel.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, 1, D3D11_SDK_VERSION, &device, &level, &context);
            if (SUCCEEDED(hr) && level < D3D_FEATURE_LEVEL_11_0) hr = E_NOTIMPL;
            ComPtr<IDXGIDevice> dxgi;
            if (SUCCEEDED(hr)) hr = device.As(&dxgi);
            if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition));
            if (SUCCEEDED(hr)) hr = compositor.initialize(device.Get(), context.Get());
            if (SUCCEEDED(hr)) compositor.set_filter_executor(&filters);
            ComPtr<ID3DBlob> program;
            if (SUCCEEDED(hr)) hr = D3DCompile(transfer_shader, sizeof(transfer_shader), nullptr, nullptr, nullptr,
                "vertex", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &program, nullptr);
            if (SUCCEEDED(hr)) hr = device->CreateVertexShader(program->GetBufferPointer(), program->GetBufferSize(), nullptr, &vertex);
            program.Reset();
            if (SUCCEEDED(hr)) hr = D3DCompile(transfer_shader, sizeof(transfer_shader), nullptr, nullptr, nullptr,
                "pixel", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &program, nullptr);
            if (SUCCEEDED(hr)) hr = device->CreatePixelShader(program->GetBufferPointer(), program->GetBufferSize(), nullptr, &pixel);
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.Usage = D3D11_USAGE_DEFAULT;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (SUCCEEDED(hr)) hr = device->CreateBuffer(&buffer, nullptr, &transfer);
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&raster, &rasterizer);
            D3D11_DEPTH_STENCIL_DESC depth_description{};
            if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&depth_description, &depth);
            if (SUCCEEDED(hr)) hr = make_query(device.Get(), presentation_completion);
            LARGE_INTEGER clock{};
            if (SUCCEEDED(hr) && (!QueryPerformanceFrequency(&clock) || clock.QuadPart <= 0)) hr = E_FAIL;
            frequency = clock.QuadPart;
            return hr;
        }

        HRESULT fence(scene_bundle*& bundle) {
            if (!bundle) return S_OK;
            if (!bundle->worker_completion) {
                const HRESULT hr = make_query(device.Get(), bundle->worker_completion);
                if (FAILED(hr)) return hr;
            }
            auto unused = std::find(pending.begin(), pending.end(), nullptr);
            if (unused == pending.end()) return E_UNEXPECTED;
            context->End(bundle->worker_completion.Get());
            context->Flush();
            *unused = bundle;
            bundle = nullptr;
            return S_OK;
        }

        HRESULT poll() {
            for (auto*& bundle : pending) {
                if (!bundle) continue;
                const HRESULT hr = query_ready(context.Get(), bundle->worker_completion.Get());
                if (hr == S_FALSE) continue;
                if (FAILED(hr)) return hr;
                release_bundle(*bundle);
                bundle = nullptr;
            }
            return S_OK;
        }

        HRESULT open(scene_bundle& bundle) {
            for (uint32_t i = 0; i < bundle.snapshot.frame.image_count + 2; ++i) {
                auto& image = *bundle.images[i];
                if (image.worker) continue;
                HRESULT hr = device->OpenSharedResource(image.handle, IID_PPV_ARGS(&image.worker));
                if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(image.worker.Get(), nullptr, &image.view);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        }

        HRESULT surface(uint32_t w, uint32_t h, DXGI_FORMAT format, bool opaque,
            ComPtr<IDCompositionSurface>& output, ComPtr<IDCompositionVisual>& visual) {
            HRESULT hr = composition->CreateSurface(w, h, format,
                opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, &output);
            if (SUCCEEDED(hr)) hr = composition->CreateVisual(&visual);
            if (SUCCEEDED(hr)) hr = visual->SetContent(output.Get());
            return hr;
        }

        HRESULT surfaces() {
            const auto count = current->snapshot.frame.image_count;
            const DXGI_FORMAT next_world = current->images[count]->format;
            const DXGI_FORMAT next_hud = current->images[count + 1]->format;
            if (background_surface) {
                return width == desired.width && height == desired.height && world_format == next_world &&
                    hud_format == next_hud ? S_OK : E_INVALIDARG;
            }
            width = desired.width;
            height = desired.height;
            world_format = next_world;
            hud_format = next_hud;
            HRESULT hr = surface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, true, background_surface, background);
            if (SUCCEEDED(hr)) hr = surface(width, height, world_format, false, world_surface, world);
            if (SUCCEEDED(hr)) hr = surface(width, height, hud_format, false, hud_surface, hud);
            return hr;
        }

        HRESULT update_surface(IDCompositionSurface* surface, ID3D11Texture2D* image, bool rendered) {
            POINT offset{};
            ComPtr<IDXGISurface> update;
            HRESULT hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&update), &offset);
            if (FAILED(hr)) return hr;
            ComPtr<IDXGISurface2> update2;
            ComPtr<ID3D11Texture2D> destination;
            UINT subresource = 0;
            hr = update.As(&update2);
            if (SUCCEEDED(hr)) hr = update2->GetResource(IID_PPV_ARGS(&destination), &subresource);
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC description{};
                destination->GetDesc(&description);
                ComPtr<ID3D11Device> owner;
                destination->GetDevice(&owner);
                const UINT mip = description.MipLevels ? subresource % description.MipLevels : 32;
                const UINT w = mip < 32 ? std::max(1u, description.Width >> mip) : 0;
                const UINT h = mip < 32 ? std::max(1u, description.Height >> mip) : 0;
                if (owner.Get() != device.Get() || subresource >= uint64_t(description.MipLevels) * description.ArraySize ||
                    description.SampleDesc.Count != 1 || description.SampleDesc.Quality || offset.x < 0 || offset.y < 0 ||
                    uint64_t(offset.x) + width > w || uint64_t(offset.y) + height > h) hr = E_INVALIDARG;
            }
            if (SUCCEEDED(hr) && rendered) {
                ComPtr<ID3D11RenderTargetView> target;
                D3D11_RENDER_TARGET_VIEW_DESC description{};
                description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                description.Texture2D.MipSlice = subresource;
                hr = device->CreateRenderTargetView(destination.Get(), &description, &target);
                if (SUCCEEDED(hr)) {
                    const float values[]{static_cast<float>(offset.x), static_cast<float>(offset.y), 0, 0};
                    context->UpdateSubresource(transfer.Get(), 0, nullptr, values, 0, 0);
                    ID3D11Buffer* constants = transfer.Get();
                    ID3D11ShaderResourceView* source = compositor.shader_resource();
                    ID3D11RenderTargetView* output = target.Get();
                    const D3D11_VIEWPORT viewport{static_cast<float>(offset.x), static_cast<float>(offset.y),
                        static_cast<float>(width), static_cast<float>(height), 0, 1};
                    context->IASetInputLayout(nullptr);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    context->VSSetShader(vertex.Get(), nullptr, 0);
                    context->PSSetShader(pixel.Get(), nullptr, 0);
                    context->PSSetConstantBuffers(0, 1, &constants);
                    context->PSSetShaderResources(0, 1, &source);
                    context->RSSetState(rasterizer.Get());
                    context->RSSetViewports(1, &viewport);
                    context->OMSetDepthStencilState(depth.Get(), 0);
                    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
                    context->OMSetRenderTargets(1, &output, nullptr);
                    context->Draw(3, 0);
                    source = nullptr;
                    context->PSSetShaderResources(0, 1, &source);
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                }
            } else if (SUCCEEDED(hr)) {
                const D3D11_BOX box{0, 0, 0, width, height, 1};
                context->CopySubresourceRegion(destination.Get(), subresource, static_cast<UINT>(offset.x),
                    static_cast<UINT>(offset.y), 0, image, 0, &box);
            }
            destination.Reset();
            update2.Reset();
            update.Reset();
            const HRESULT end = surface->EndDraw();
            return FAILED(hr) ? hr : end;
        }
    };

    scene_worker::scene_worker() {
        try {
            state.reset(new (std::nothrow) implementation());
        } catch (const std::bad_alloc&) {
            state.reset();
        }
    }
    scene_worker::~scene_worker() = default;

    HRESULT scene_worker::prepare(scene_channel& channel, const smf_bridge_desired* request) {
        if (!state) return E_OUTOFMEMORY;
        auto& s = *state;
        s.dirty = false;
        s.reused = false;
        if (channel.retiring.load()) return S_FALSE;
        HRESULT hr = s.context ? s.poll() : S_OK;
        if (FAILED(hr)) return hr;
        scene_bundle* next = channel.acquire();
        if (next) {
            if (!s.device) hr = s.initialize(channel);
            if (SUCCEEDED(hr)) hr = s.fence(s.current);
            if (FAILED(hr)) {
                release_bundle(*next);
                return hr;
            }
            s.current = next;
            hr = s.open(*next);
        }
        if (FAILED(hr) || !s.current) return FAILED(hr) ? hr : S_FALSE;
        const auto& frame = s.current->frame;
        const auto& camera = s.current->camera;
        affine actual{};
        if (!projection(frame.pose, camera.width, camera.height, actual)) return E_INVALIDARG;
        const smf_scene::layer* live = nullptr;
        for (uint32_t i = 0; i < s.current->snapshot.frame.layer_count; ++i) {
            if (s.current->snapshot.layers[i].kind == smf_scene::live_map) live = &s.current->snapshot.layers[i];
        }
        if (!live) return E_INVALIDARG;
        actual = {live->affine[0], live->affine[1], live->affine[2], live->affine[3], live->affine[4], live->affine[5]};
        double x = frame.pose.x;
        double z = frame.pose.z;
        affine desired = actual;
        if (request) {
            if (request->epoch != camera.camera_epoch || request->map_id != camera.map_id) return S_FALSE;
            const double shake_x = frame.pose.x - frame.pose.root_x;
            const double shake_z = frame.pose.z - frame.pose.root_z;
            x = request->x + shake_x;
            z = request->z + shake_z;
            if (request->x != frame.pose.root_x || request->z != frame.pose.root_z ||
                request->projection_half_height != frame.pose.orthographic_size) {
                model captured = camera;
                captured.nominal = actual;
                captured.x = frame.pose.root_x;
                captured.z = frame.pose.root_z;
                captured.projection_half_height = frame.pose.orthographic_size;
                if (!root_projection(captured, request->x, request->z, request->projection_half_height, desired)) return E_INVALIDARG;
            }
        }
        s.desired.width = camera.width;
        s.desired.height = camera.height;
        const double transform[]{desired.a, desired.b, desired.c, desired.d, desired.e, desired.f};
        std::copy(std::begin(transform), std::end(transform), s.desired.map_affine);
        s.desired.camera_x = x;
        s.desired.camera_z = z;
        if (!mapping(desired, actual, camera.height, (frame.flags & session_world_flip_y) != 0, s.world_transform)) return E_INVALIDARG;
        if (s.displayed_frame == frame.source_frame && std::memcmp(&s.desired, &s.displayed, sizeof(s.desired)) == 0) {
            s.reused = true;
            return S_FALSE;
        }
        const int64_t timestamp = now();
        s.update_cadence(timestamp);
        if (s.displayed_frame && timestamp < s.next_frame) return S_FALSE;
        if (s.presentation_pending) {
            hr = query_ready(s.context.Get(), s.presentation_completion.Get());
            if (hr != S_OK) return hr;
            s.presentation_pending = false;
        }
        hr = s.surfaces();
        std::array<ID3D11ShaderResourceView*, smf_scene::maximum_images> views{};
        for (uint32_t i = 0; i < s.current->snapshot.frame.image_count; ++i) views[i] = s.current->images[i]->view.Get();
        const auto& snapshot = s.current->snapshot;
        const smf_scene::frame input{&snapshot.frame, snapshot.images.data(), snapshot.layers.data(), snapshot.effects.data(), views.data()};
        if (SUCCEEDED(hr)) hr = s.compositor.draw(input, s.desired);
        if (SUCCEEDED(hr)) s.dirty = true;
        return hr;
    }

    HRESULT scene_worker::present(scene_channel& channel) {
        if (!state || !state->current || channel.retiring.load()) return S_FALSE;
        auto& s = *state;
        if (s.displayed_frame && !s.ready) {
            if (!s.initial_completion)
                s.initial_completion = completion_request(s.composition.Get(), s.displayed_frame, s.current->frame.session);
            if (s.initial_completion) {
                uint64_t completed = 0;
                const HRESULT hr = completion_poll(s.initial_completion, &completed);
                if (FAILED(hr)) return hr;
                if (hr == S_OK) {
                    s.initial_completion = 0;
                    s.ready = true;
                    channel.background = s.background;
                    channel.world = s.world;
                    channel.hud = s.hud;
                    channel.prepared_frame.store(completed, std::memory_order_release);
                    channel.presented_frame.store(s.displayed_frame, std::memory_order_release);
                }
            }
        }
        if (!s.dirty) return S_FALSE;
        const auto& frame = s.current->frame;
        const auto count = s.current->snapshot.frame.image_count;
        HRESULT hr = s.update_surface(s.background_surface.Get(), nullptr, true);
        if (SUCCEEDED(hr) && s.displayed_frame != frame.source_frame)
            hr = s.update_surface(s.world_surface.Get(), s.current->images[count]->worker.Get(), false);
        if (SUCCEEDED(hr) && s.displayed_frame != frame.source_frame)
            hr = s.update_surface(s.hud_surface.Get(), s.current->images[count + 1]->worker.Get(), false);
        if (SUCCEEDED(hr)) hr = s.world->SetTransform(s.world_transform);
        const bool flip = (frame.flags & session_hud_flip_y) != 0;
        if (SUCCEEDED(hr)) hr = s.hud->SetTransform(D2D_MATRIX_3X2_F{1, 0, 0, flip ? -1.f : 1.f, 0, flip ? static_cast<float>(s.height) : 0});
        if (SUCCEEDED(hr)) hr = s.composition->Commit();
        if (FAILED(hr)) return hr;
        s.context->End(s.presentation_completion.Get());
        s.context->Flush();
        s.presentation_pending = true;
        s.next_frame = now() + s.interval;
        s.displayed = s.desired;
        s.displayed_frame = frame.source_frame;
        s.dirty = false;
        if (s.ready) channel.presented_frame.store(frame.source_frame, std::memory_order_release);
        return S_OK;
    }

    HRESULT scene_worker::drain(scene_channel& channel) {
        channel.discard_ready();
        if (!state || !state->context) return S_OK;
        state->dirty = false;
        return state->poll();
    }

    HRESULT scene_worker::retire(scene_channel& channel) {
        if (!state || !state->device) {
            channel.worker_retired = true;
            return S_OK;
        }
        auto& s = *state;
        if (s.initial_completion) {
            const HRESULT completed = completion_poll(s.initial_completion);
            if (completed == S_FALSE) return S_FALSE;
            s.initial_completion = 0;
            if (FAILED(completed)) return completed;
        }
        HRESULT hr = s.fence(s.current);
        if (SUCCEEDED(hr)) hr = s.poll();
        if (FAILED(hr)) return hr;
        if (std::any_of(s.pending.begin(), s.pending.end(), [](auto* frame) { return frame != nullptr; })) return S_FALSE;
        s.compositor.release();
        channel.worker_retired.store(true, std::memory_order_release);
        return S_OK;
    }

    bool scene_worker::prepared() const { return state && state->ready; }
    bool scene_worker::changed() const { return state && state->dirty; }
    bool scene_worker::reused_pose() const { return state && state->reused; }
    uint64_t scene_worker::source_frame() const { return state && state->current ? state->current->frame.source_frame : 0; }

}
