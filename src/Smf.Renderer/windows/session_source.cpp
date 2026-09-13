#include "session_internal.h"
#include "present_observer.h"
#include "scene_transport.h"
#include <algorithm>
#include <cstring>

// Everything here runs on Unity's render thread.
namespace session {
    namespace {

        struct layer {
            ComPtr<IDCompositionSurface> surface;
            ComPtr<IDCompositionVisual> visual;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        };

        struct generation {
            uint64_t staged_frame = 0;
            uint64_t post_attach = 0;
            uint64_t post_attach_frame = 0;
            uint64_t completion = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t flags = 0;
            ComPtr<ID3D11Texture2D> staging;
            layer base;
            layer world;
            layer hud;
            layer cache;
            layer background;
            session_cache cache_description{};
            ComPtr<ID3D11Texture2D> cache_source; // pinned so the same texture can be recognised across serials
            ComPtr<IDCompositionVisual> map_group;
            ComPtr<IDCompositionVisual> hud_group;
            ComPtr<IDCompositionVisual> background_group;
            ComPtr<IDCompositionEffectGroup> map_opacity;
            ComPtr<IDCompositionEffectGroup> hud_opacity;
            ComPtr<IDCompositionEffectGroup> background_opacity;
            model model{};
            bool poisoned = false;
            bool scene = false;
        };

        struct source_state {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            ComPtr<IDCompositionDevice> composition;
            std::array<generation, generation_count> slots{};
            present_context observation{};
            uint64_t begun = 0;
            bool poisoned = false;
        };

        // Leaked on purpose; released only by explicit retirement.
        source_state& source = *new source_state();
        constexpr D2D_MATRIX_3X2_F identity_matrix{1, 0, 0, 1, 0, 0};

        HRESULT bootstrap(ID3D11Texture2D* texture) {
            ComPtr<ID3D11Device> device;
            texture->GetDevice(&device);
            if (source.device) return source.device.Get() == device.Get() ? S_OK : E_INVALIDARG;

            ComPtr<IDXGIDevice> dxgi;
            HRESULT hr = device.As(&dxgi);
            if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&source.composition));
            if (FAILED(hr)) return hr;

            source.device = device;
            device->GetImmediateContext(&source.context);
            return S_OK;
        }

        HRESULT check_texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height, DXGI_FORMAT& format) {
            if (!texture) return E_POINTER;

            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            ComPtr<ID3D11Device> owner;
            texture->GetDevice(&owner);
            format = compatible_format(d.Format);

            const bool ok = owner.Get() == source.device.Get() && d.Width == width && d.Height == height &&
                d.MipLevels == 1 && d.ArraySize == 1 && d.SampleDesc.Count == 1 && !d.SampleDesc.Quality &&
                format != DXGI_FORMAT_UNKNOWN;
            return ok ? S_OK : E_INVALIDARG;
        }

        HRESULT current_backbuffer(ComPtr<ID3D11Texture2D>& texture) {
            if (!source.context) return E_PENDING;

            ComPtr<ID3D11RenderTargetView> target;
            source.context->OMGetRenderTargets(1, &target, nullptr);
            if (!target) return E_PENDING;
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            target->GetDesc(&view);
            if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice) return E_INVALIDARG;

            ComPtr<ID3D11Resource> resource;
            target->GetResource(&resource);
            return resource.As(&texture);
        }

        HRESULT make_layer(layer& l, uint32_t w, uint32_t h, DXGI_FORMAT format, bool opaque) {
            HRESULT hr = source.composition->CreateSurface(w, h, format, opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, &l.surface);
            if (SUCCEEDED(hr)) hr = source.composition->CreateVisual(&l.visual);
            if (SUCCEEDED(hr)) hr = l.visual->SetContent(l.surface.Get());
            l.format = format;
            return hr;
        }

        HRESULT copy_into(layer& l, ID3D11Texture2D* texture, uint32_t width, uint32_t height, uint32_t source_x = 0, uint32_t source_y = 0) {
            if (source_x || source_y) {
                D3D11_TEXTURE2D_DESC src{};
                texture->GetDesc(&src);
                if (!copy_region_fits(src.Width, src.Height, source_x, source_y, width, height)) return E_INVALIDARG;
            }

            POINT offset{};
            ComPtr<IDXGISurface> update;
            HRESULT hr = l.surface->BeginDraw(nullptr, IID_PPV_ARGS(&update), &offset);
            if (FAILED(hr)) return hr;

            ComPtr<IDXGISurface2> update2;
            ComPtr<ID3D11Texture2D> destination;
            UINT subresource = 0;
            hr = update.As(&update2);
            if (SUCCEEDED(hr)) hr = update2->GetResource(IID_PPV_ARGS(&destination), &subresource);

            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC d{};
                destination->GetDesc(&d);
                ComPtr<ID3D11Device> owner;
                destination->GetDevice(&owner);
                const UINT mip = d.MipLevels ? subresource % d.MipLevels : 32;
                const UINT w = mip < 32 ? std::max(1u, d.Width >> mip) : 0;
                const UINT h = mip < 32 ? std::max(1u, d.Height >> mip) : 0;

                if (owner.Get() != source.device.Get() || !d.MipLevels || subresource >= uint64_t(d.MipLevels) * d.ArraySize ||
                    d.SampleDesc.Count != 1 || d.SampleDesc.Quality || compatible_format(d.Format) != l.format ||
                    offset.x < 0 || offset.y < 0 || uint64_t(offset.x) + width > w || uint64_t(offset.y) + height > h ||
                    destination.Get() == texture) {
                    hr = E_INVALIDARG;
                } else {
                    const D3D11_BOX box{source_x, source_y, 0, source_x + width, source_y + height, 1};
                    source.context->CopySubresourceRegion(destination.Get(), subresource, static_cast<UINT>(offset.x),
                        static_cast<UINT>(offset.y), 0, texture, 0, &box);
                }
            }

            destination.Reset();
            update2.Reset();
            update.Reset();

            const HRESULT end = l.surface->EndDraw();
            return FAILED(hr) ? hr : end;
        }

        HRESULT create_layers(generation& g, const session_generation_status& status, DXGI_FORMAT base, DXGI_FORMAT world,
            DXGI_FORMAT hud, const session_frame& frame, bool scene = false) {
            g.width = status.width;
            g.height = status.height;
            g.flags = status.flags;
            g.scene = scene;
            const bool cached = !scene && frame.cache.serial != 0;

            HRESULT hr = scene ? S_OK : make_layer(g.base, g.width, g.height, base, true);
            if (SUCCEEDED(hr) && cached) hr = make_layer(g.cache, frame.cache.width, frame.cache.height, base, true);
            if (SUCCEEDED(hr) && cached) hr = make_layer(g.background, 1, 1, base, true);
            if (SUCCEEDED(hr) && !scene) hr = make_layer(g.world, g.width, g.height, world, false);
            if (SUCCEEDED(hr) && !scene) hr = make_layer(g.hud, g.width, g.height, hud, false);
            if (SUCCEEDED(hr)) hr = source.composition->CreateVisual(&g.map_group);
            if (SUCCEEDED(hr)) hr = source.composition->CreateVisual(&g.hud_group);
            if (SUCCEEDED(hr)) hr = source.composition->CreateVisual(&g.background_group);
            if (SUCCEEDED(hr)) hr = source.composition->CreateEffectGroup(&g.map_opacity);
            if (SUCCEEDED(hr)) hr = source.composition->CreateEffectGroup(&g.hud_opacity);
            if (SUCCEEDED(hr)) hr = source.composition->CreateEffectGroup(&g.background_opacity);
            if (SUCCEEDED(hr)) hr = g.map_opacity->SetOpacity(0.f);
            if (SUCCEEDED(hr)) hr = g.hud_opacity->SetOpacity(0.f);
            if (SUCCEEDED(hr)) hr = g.background_opacity->SetOpacity(0.f);
            if (SUCCEEDED(hr)) hr = g.map_group->SetEffect(g.map_opacity.Get());
            if (SUCCEEDED(hr)) hr = g.hud_group->SetEffect(g.hud_opacity.Get());
            if (SUCCEEDED(hr)) hr = g.background_group->SetEffect(g.background_opacity.Get());
            if (scene) return hr;

            if (SUCCEEDED(hr) && cached) {
                // The 1x1 background stretches over the whole viewport.
                const D2D_MATRIX_3X2_F viewport{static_cast<float>(g.width), 0, 0, static_cast<float>(g.height), 0, 0};
                hr = g.background.visual->SetTransform(viewport);
                if (SUCCEEDED(hr)) hr = g.background_group->AddVisual(g.background.visual.Get(), FALSE, nullptr);
            }

            if (SUCCEEDED(hr) && cached) hr = g.map_group->AddVisual(g.cache.visual.Get(), FALSE, nullptr);
            if (SUCCEEDED(hr)) hr = g.map_group->AddVisual(g.base.visual.Get(), FALSE, nullptr);
            if (SUCCEEDED(hr)) hr = g.map_group->AddVisual(g.world.visual.Get(), TRUE, g.base.visual.Get());
            if (SUCCEEDED(hr)) hr = g.hud_group->AddVisual(g.hud.visual.Get(), FALSE, nullptr);
            return hr;
        }

        HRESULT commit(uint64_t& serial) {
            if (source.poisoned) return E_FAIL;

            const HRESULT hr = source.composition->Commit();
            if (hr == S_OK) serial = ++state().status.source_commit;
            return hr;
        }

        // After a partial update nothing is committed again; the last complete bundle stays
        // on screen until the generation is retired.
        void record_failure(size_t i, HRESULT hr) {
            auto& s = state();

            s.status.result = hr;
            s.status.generations[i].state = 6;
            source.slots[i].poisoned = true;
            ++s.status.dropped_frames;
            source.poisoned = true;
        }

        void begin_frame(uint64_t frame) {
            if (source.observation.session && frame > source.begun) {
                smf_po_begin_source_frame(source.observation.session, source.observation.epoch, frame);
                source.begun = frame;
            }
        }

        void native_marker(const session_native_frame& frame) {
            auto& s = state();
            if (!source.context) return; // retired: never bring the observer back

            if (frame.session == s.session.load() && (frame.flags & 1u)) {
                begin_frame(frame.source_frame);
                return;
            }

            if (frame.session != s.session.load() || (frame.source_frame <= s.restore_after_frame.load() && frame.restore_serial)) return;
            if (frame.restore_serial != s.operation_fence.load()) return;

            auto& c = source.observation;
            if (c.session != frame.session || c.restore_serial != frame.restore_serial ||
                c.content_generation != frame.content_revision || c.resource_generation != frame.generation) {
                present_context next{sizeof(next), 1, frame.session, ++s.observer_epoch, frame.restore_serial,
                    frame.restore_serial ? s.restore_after_frame.load() : 0, frame.content_revision, frame.generation};
                if (!smf_po_context(&next)) return;
                c = next;
                source.begun = 0;
                s.status.flags &= ~2u;
            }

            begin_frame(frame.source_frame);
            ComPtr<ID3D11Texture2D> backbuffer;
            DXGI_FORMAT format;
            if (FAILED(current_backbuffer(backbuffer)) || FAILED(check_texture(backbuffer.Get(), frame.width, frame.height, format))) return;
            present_marker marker{sizeof(marker), 1, c.session, c.epoch, c.restore_serial, frame.source_frame,
                c.content_generation, c.resource_generation};

            // Unity's camera target is not always the DXGI backbuffer (traces show a distinct
            // target shortly before Present), so the marker is matched in Unity-target mode.
            if (!smf_po_rendered_unity_target(&marker, backbuffer.Get())) return;

            s.status.native_ordered_frame = frame.source_frame;
            s.status.native_ordered_restore_serial = frame.restore_serial;
        }

        void poll_native() {
            auto& s = state();
            if (s.status.native_submitted_frame <= s.native_reject_through_frame.load()) s.status.flags &= ~2u;
            if (!source.observation.session) return;

            present_event event{};
            event.size = sizeof(event);
            event.version = 1;
            if (!smf_po_last_submission(&source.observation, &event)) return;

            // A marker from before a dropped expiry packet must not stand for a later frame.
            if (event.source_frame <= s.native_reject_through_frame.load()) {
                s.status.flags &= ~2u;
                return;
            }

            s.status.native_submitted_frame = event.source_frame;
            s.status.native_submitted_generation = event.resource_generation;
            s.status.native_submitted_content = event.content_generation;
            s.status.native_submitted_restore_serial = event.restore_serial;
            s.status.native_present_serial = event.sequence;
            s.status.native_backbuffer = event.buffer_identity;
            // "Reveal" only means the native Present returned S_OK, not that it was displayed.
            s.status.native_reveal_frame = event.source_frame;
            s.status.flags |= 2u;
        }

        HRESULT stage_pre_gui(generation& g, session_generation_status& status, const ticket& t) {
            HRESULT hr = bootstrap(t.first);
            if (FAILED(hr)) return hr;

            begin_frame(t.pre.source_frame);
            ComPtr<ID3D11Texture2D> backbuffer;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            hr = current_backbuffer(backbuffer);
            if (SUCCEEDED(hr)) hr = check_texture(backbuffer.Get(), status.width, status.height, format);

            if (SUCCEEDED(hr) && !g.staging) {
                D3D11_TEXTURE2D_DESC d{};
                backbuffer->GetDesc(&d);
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = 0;
                d.CPUAccessFlags = 0;
                d.MiscFlags = 0;
                hr = source.device->CreateTexture2D(&d, nullptr, &g.staging);
            }

            if (SUCCEEDED(hr)) {
                DXGI_FORMAT old;
                hr = check_texture(g.staging.Get(), status.width, status.height, old);
                if (SUCCEEDED(hr) && old != format) hr = E_INVALIDARG;
            }

            if (SUCCEEDED(hr)) {
                if (!g.scene) source.context->CopyResource(g.staging.Get(), backbuffer.Get());
                g.staged_frame = t.pre.source_frame;
                status.staged_frame = t.pre.source_frame;
            }

            return hr;
        }

        void present_scene(size_t index, generation& g, session_generation_status& status, const ticket& t) {
            auto& s = state();
            const auto& f = t.frame;
            affine actual{};
            HRESULT hr = projection(f.pose, status.width, status.height, actual) ? S_OK : E_INVALIDARG;
            if (SUCCEEDED(hr) && !g.model.valid) {
                g.model.nominal = actual;
                g.model.nominal.c += actual.a * (f.pose.x - f.pose.root_x) + actual.b * (f.pose.z - f.pose.root_z);
                g.model.nominal.f += actual.d * (f.pose.x - f.pose.root_x) + actual.e * (f.pose.z - f.pose.root_z);
                g.model.x = f.pose.root_x;
                g.model.z = f.pose.root_z;
                g.model.projection_half_height = f.pose.orthographic_size;
                g.model.epoch = f.pose.epoch;
                g.model.revision = f.pose.model_revision;
                g.model.camera_epoch = f.pose.camera_epoch;
                g.model.map_id = f.pose.map_id;
                g.model.width = status.width;
                g.model.height = status.height;
                g.model.valid = true;
            }
            affine nominal{};
            if (SUCCEEDED(hr) && !source_model(g.model, f.pose, actual, nominal)) hr = E_INVALIDARG;
            if (SUCCEEDED(hr) && g.map_group && !g.scene) hr = E_INVALIDARG;
            if (SUCCEEDED(hr) && !g.map_group)
                hr = create_layers(g, status, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, f, true);
            if (SUCCEEDED(hr) && !s.scenes[index]) {
                for (const auto& channel : s.scenes) {
                    if (channel) channel->trim();
                }
                try {
                    auto channel = std::make_shared<scene_channel>();
                    hr = channel->initialize(source.device.Get(), source.context.Get());
                    if (SUCCEEDED(hr)) s.scenes[index] = std::move(channel);
                } catch (const std::bad_alloc&) {
                    hr = E_OUTOFMEMORY;
                }
            }
            g.model.camera_epoch = f.pose.camera_epoch;
            if (SUCCEEDED(hr)) hr = s.scenes[index]->submit(t.scene, f, g.model);
            if (FAILED(hr)) {
                record_failure(index, hr);
                return;
            }
            if (hr == S_FALSE) {
                ++s.status.dropped_frames;
                return;
            }
            ++status.frames;
            status.last_source_frame = f.source_frame;
            status.staged_frame = 0;
            g.staged_frame = 0;
            s.models[index] = g.model;
            wake();
        }

        void present_frame(size_t index, generation& g, session_generation_status& status, const ticket& t,
            uint64_t generation_id, uint64_t content) {
            auto& s = state();
            const auto& f = t.frame;
            if (t.has_scene) {
                present_scene(index, g, status, t);
                return;
            }
            if (g.scene) {
                record_failure(index, E_INVALIDARG);
                return;
            }

            DXGI_FORMAT base = DXGI_FORMAT_UNKNOWN;
            DXGI_FORMAT world = DXGI_FORMAT_UNKNOWN;
            DXGI_FORMAT hud = DXGI_FORMAT_UNKNOWN;
            HRESULT hr = check_texture(g.staging.Get(), status.width, status.height, base);
            if (SUCCEEDED(hr)) hr = check_texture(t.first, status.width, status.height, world);
            if (SUCCEEDED(hr)) hr = check_texture(t.second, status.width, status.height, hud);

            const cache_update update = decide_cache_update(g.cache_description, f.cache);
            if (!cache_valid(f) || update == cache_update::reject) {
                // Drop the whole bundle rather than let a stale cache poison later frames.
                ++s.status.dropped_frames;
                return;
            }

            if (SUCCEEDED(hr) && update == cache_update::copy) {
                DXGI_FORMAT cache_format = DXGI_FORMAT_UNKNOWN;
                hr = check_texture(t.cache, f.cache.width, f.cache.height, cache_format);
                if (SUCCEEDED(hr) && (cache_format != base || t.cache == g.staging.Get())) hr = E_INVALIDARG;
            } else if (SUCCEEDED(hr) && f.cache.serial && t.cache != g.cache_source.Get()) {
                hr = E_INVALIDARG;
            }

            affine actual{};
            affine nominal{};
            D2D_MATRIX_3X2_F map_transform = identity_matrix;
            D2D_MATRIX_3X2_F cache_transform = identity_matrix;

            if (SUCCEEDED(hr) && (f.flags & session_has_map)) {
                if (!projection(f.pose, status.width, status.height, actual)) {
                    hr = E_INVALIDARG;
                } else {
                    if (!g.model.valid) {
                        g.model.nominal = actual;
                        g.model.nominal.c += actual.a * (f.pose.x - f.pose.root_x) + actual.b * (f.pose.z - f.pose.root_z);
                        g.model.nominal.f += actual.d * (f.pose.x - f.pose.root_x) + actual.e * (f.pose.z - f.pose.root_z);
                        g.model.x = f.pose.root_x;
                        g.model.z = f.pose.root_z;
                        g.model.projection_half_height = f.pose.orthographic_size;
                        g.model.epoch = f.pose.epoch;
                        g.model.revision = f.pose.model_revision;
                        g.model.camera_epoch = f.pose.camera_epoch;
                        g.model.map_id = f.pose.map_id;
                        g.model.width = status.width;
                        g.model.height = status.height;
                        g.model.valid = true;
                    }

                    if (!source_model(g.model, f.pose, actual, nominal) || !mapping(g.model.nominal, nominal, status.height, false, map_transform)) {
                        hr = E_INVALIDARG;
                    }

                    // Worker D*inv(N0), source N0*inv(N) and cache N*inv(C) compose to D*inv(C).
                    const auto& c = f.cache.affine;
                    const affine cache_affine{c[0], c[1], c[2], c[3], c[4], c[5]};
                    if (SUCCEEDED(hr) && !mapping(nominal, cache_affine, f.cache.height, (f.cache.flags & 2u) != 0, cache_transform)) {
                        hr = E_INVALIDARG;
                    }
                }
            }

            if (SUCCEEDED(hr) && !g.map_group) hr = create_layers(g, status, base, world, hud, f);
            if (SUCCEEDED(hr) && (g.base.format != base || g.world.format != world || g.hud.format != hud)) hr = E_INVALIDARG;
            if (SUCCEEDED(hr) && update == cache_update::copy) hr = copy_into(g.cache, t.cache, f.cache.width, f.cache.height);
            // Logical corner (0, 0) of the cache holds the clear colour; one texel of it feeds the background.
            if (SUCCEEDED(hr) && update == cache_update::copy) hr = copy_into(g.background, t.cache, 1, 1, 0, cache_background_source_y(f.cache));
            if (SUCCEEDED(hr)) hr = copy_into(g.base, g.staging.Get(), status.width, status.height);
            if (SUCCEEDED(hr)) hr = copy_into(g.world, t.first, status.width, status.height);
            if (SUCCEEDED(hr)) hr = copy_into(g.hud, t.second, status.width, status.height);

            const auto orientation = [&](bool flip) {
                return D2D_MATRIX_3X2_F{1, 0, 0, flip ? -1.f : 1.f, 0, flip ? static_cast<float>(status.height) : 0};
            };

            if (SUCCEEDED(hr)) hr = g.map_group->SetTransform(map_transform);
            if (SUCCEEDED(hr) && f.cache.serial) hr = g.cache.visual->SetTransform(cache_transform);
            if (SUCCEEDED(hr)) hr = g.world.visual->SetTransform(orientation((f.flags & session_world_flip_y) != 0));
            if (SUCCEEDED(hr)) hr = g.hud.visual->SetTransform(orientation((f.flags & session_hud_flip_y) != 0));
            if (SUCCEEDED(hr)) hr = commit(status.source_commit); // cache, base, world and HUD land together
            if (FAILED(hr)) {
                record_failure(index, hr);
                return;
            }

            if (update == cache_update::copy) {
                g.cache_description = f.cache;
                g.cache_source = t.cache;
            }

            ++status.frames;
            status.last_source_frame = f.source_frame;
            status.base_format = base;
            status.world_format = world;
            status.hud_format = hud;
            if (g.model.valid) g.model.camera_epoch = f.pose.camera_epoch;
            status.staged_frame = 0;
            g.staged_frame = 0;
            s.models[index] = g.model;
            if (status.state == 3) s.status.active_frame = f.source_frame;

            if (!s.links[index].serial) {
                auto& l = s.links[index];
                l.generation = generation_id;
                l.content = content;
                l.background = g.background_group.Get();
                l.map = g.map_group.Get();
                l.hud = g.hud_group.Get();
                l.serial = s.next_link++;
                status.attachment = l.serial;
                wake();
            }
        }

        struct outcome {
            session_command command{};
            HRESULT hr = S_OK;
            uint32_t evidence = 0;
            uint64_t frame = 0;
            uint64_t commit = 0;
            bool superseded = false;
        };

        void pump_scenes() {
            auto& s = state();
            for (size_t i = 0; i < generation_count; ++i) {
                const auto& channel = s.scenes[i];
                if (!channel) continue;
                const auto& generation = s.status.generations[i];
                const bool current = is_current(s.session.load(), generation.content_revision, generation.generation);
                HRESULT hr = channel->poll(!current);
                if (FAILED(hr)) {
                    record_failure(i, hr);
                    continue;
                }
                const uint64_t frame = channel->presented_frame.load(std::memory_order_acquire);
                auto& status = s.status.generations[i];
                auto& g = source.slots[i];
                auto& link = s.links[i];
                if (channel->retiring.load() || g.poisoned || source.poisoned || !frame) continue;
                if (status.state == 3) s.status.active_frame = frame;
                if (link.serial) continue;
                hr = g.background_group->AddVisual(channel->background.Get(), FALSE, nullptr);
                if (SUCCEEDED(hr)) hr = g.map_group->AddVisual(channel->world.Get(), FALSE, nullptr);
                if (SUCCEEDED(hr)) hr = g.hud_group->AddVisual(channel->hud.Get(), FALSE, nullptr);
                if (SUCCEEDED(hr)) hr = commit(status.source_commit);
                if (FAILED(hr)) {
                    record_failure(i, hr);
                    continue;
                }
                link.generation = status.generation;
                link.content = status.content_revision;
                link.background = g.background_group.Get();
                link.map = g.map_group.Get();
                link.hud = g.hud_group.Get();
                link.serial = s.next_link++;
                status.attachment = link.serial;
                wake();
            }
        }

        // Promotes a hidden generation to ready once a commit made after its attachment completed.
        void pump_generations() {
            auto& s = state();

            for (size_t i = 0; i < generation_count; ++i) {
                auto& status = s.status.generations[i];
                auto& g = source.slots[i];
                auto& l = s.links[i];

                if (status.state != 1 || !status.frames || !l.attached || l.acknowledged != l.serial) continue;

                status.attachment_completed = l.acknowledged;
                if (!g.post_attach) {
                    const HRESULT hr = commit(g.post_attach);
                    if (FAILED(hr)) {
                        record_failure(i, hr);
                        continue;
                    }
                    g.post_attach_frame = s.scenes[i] ? s.scenes[i]->prepared_frame.load() : status.last_source_frame;
                    status.source_commit = g.post_attach;
                }

                if (!g.completion) g.completion = completion_request(source.composition.Get(), g.post_attach, s.session.load());
                if (g.completion) {
                    uint64_t serial = 0;
                    const HRESULT hr = completion_poll(g.completion, &serial);
                    if (hr == S_FALSE) continue;
                    g.completion = 0;
                    if (FAILED(hr)) {
                        record_failure(i, hr);
                        continue;
                    }

                    status.completed_commit = serial;
                    s.status.source_completed = std::max(s.status.source_completed, serial);
                    status.prepared_frame = g.post_attach_frame;
                    status.state = 2;
                }
            }
        }

        bool pump_prepare(operation& op, size_t index, outcome& result) {
            auto& s = state();
            const auto& c = op.command;
            bool done = false;

            if (s.status.worker_state == 4) {
                done = true;
                result.hr = s.status.result;
            }

            present_status observer{};
            observer.size = sizeof(observer);
            observer.version = 1;

            if (smf_po_status(&observer) && observer.state == 3) {
                done = true;
                result.hr = observer.failure_code < 0 ? observer.failure_code : E_FAIL;
            }

            if (!op.phase) {
                for (size_t i = 0; i < generation_count; ++i) {
                    if (!s.status.generations[i].state || s.status.generations[i].state == 5) {
                        index = i;
                        break;
                    }
                }

                if (index != generation_count) {
                    auto& status = s.status.generations[index];
                    status = {};
                    status.generation = c.generation;
                    status.content_revision = c.content_revision;
                    status.width = c.width;
                    status.height = c.height;
                    status.flags = c.flags;
                    status.state = 1;
                    op.phase = 1;
                }
            }

            if (!done && index != generation_count) {
                const auto& status = s.status.generations[index];
                if (status.state == 2 || status.state == 3) {
                    done = true;
                    result.evidence = evidence_complete_composite | evidence_commit_processed;
                    result.frame = status.prepared_frame;
                    result.commit = status.completed_commit;
                } else if (status.state == 6) {
                    done = true;
                    result.hr = s.status.result;
                }
            }

            return done;
        }

        bool pump_activate(operation& op, size_t index, outcome& result) {
            auto& s = state();
            const auto& c = op.command;
            bool done = false;

            if (index == generation_count) {
                done = true;
                result.hr = E_INVALIDARG;
            } else if (!op.phase && s.status.generations[index].state == 2) {
                // All opacity groups live on the one source device, so the switch is atomic.
                HRESULT hr = S_OK;
                for (size_t i = 0; i < generation_count && SUCCEEDED(hr); ++i) {
                    auto& g = source.slots[i];
                    if (!g.map_opacity) continue;
                    hr = g.map_opacity->SetOpacity(i == index ? 1.f : 0.f);
                    if (SUCCEEDED(hr)) hr = g.hud_opacity->SetOpacity(i == index ? 1.f : 0.f);
                    if (SUCCEEDED(hr)) hr = g.background_opacity->SetOpacity(i == index ? 1.f : 0.f);
                }

                if (SUCCEEDED(hr)) hr = commit(op.completion);
                if (FAILED(hr)) {
                    done = true;
                    result.hr = hr;
                    source.poisoned = true;
                    s.status.result = hr;
                } else {
                    op.phase = 1;
                    op.frame = s.scenes[index] ? s.scenes[index]->presented_frame.load() : s.status.generations[index].last_source_frame;
                    s.status.active_generation = c.generation;
                    s.status.active_frame = op.frame;

                    for (size_t i = 0; i < generation_count; ++i) {
                        if (s.status.generations[i].state == 3) s.status.generations[i].state = 2;
                    }

                    s.status.generations[index].state = 3;
                    s.ever_active = true;
                    s.status.flags &= ~4u;
                }
            }

            if (op.phase == 1) {
                const uint64_t request = completion_request(source.composition.Get(), op.completion, c.session);
                if (request) {
                    op.phase = 2;
                    op.completion = request;
                }
            }

            if (op.phase == 2) {
                uint64_t serial = 0;
                const HRESULT hr = completion_poll(op.completion, &serial);
                if (hr != S_FALSE) {
                    done = true;
                    result.hr = hr;
                    result.commit = serial;
                    result.frame = op.frame;
                    result.evidence = evidence_composite_activated | evidence_commit_processed;
                    s.status.source_completed = std::max(s.status.source_completed, serial);
                }
            }

            return done;
        }

        bool pump_retire(const session_command& c, outcome& result) {
            auto& s = state();
            bool all = true;

            for (size_t i = 0; i < generation_count; ++i) {
                auto& status = s.status.generations[i];
                auto& g = source.slots[i];
                auto& l = s.links[i];

                if (!status.state || status.state == 5 || (c.operation == op_retire_generation && status.generation != c.generation)) continue;
                if (status.generation == s.status.active_generation && !(s.detach_requested && s.detach_completed == s.detach_requested)) {
                    all = false;
                    continue;
                }

                status.state = 4;
                status.retire_requested = c.serial;
                if (s.scenes[i]) s.scenes[i]->retiring.store(true);
                bool queued = false;
                for (const auto& t : s.tickets) {
                    const uint64_t ticket_generation = t.kind == ticket_kind::pre_gui ? t.pre.generation : t.frame.generation;
                    if (ticket_holds_generation(static_cast<uint32_t>(t.kind), ticket_generation, status.generation)) {
                        queued = true;
                        break;
                    }
                }

                if (l.background || l.map || l.hud) {
                    l.background = nullptr;
                    l.map = nullptr;
                    l.hud = nullptr;
                    l.serial = s.next_link++;
                    wake();
                }

                if (g.completion) {
                    const HRESULT hr = completion_poll(g.completion);
                    if (hr != S_FALSE) g.completion = 0;
                }

                const bool scene_pending = s.scenes[i] && (!s.scenes[i]->worker_retired.load() || !s.scenes[i]->idle());
                if (queued || (l.serial && l.acknowledged != l.serial) || g.completion || scene_pending) {
                    all = false;
                    continue;
                }

                g = {};
                s.scenes[i].reset();
                s.models[i] = {};
                l = {};
                status.state = 5;
                status.retired_serial = c.serial;
            }

            if (!all) return false;
            if (c.operation == op_retire_session) {
                for (const auto& t : s.tickets) {
                    if (t.kind != ticket_kind::empty) return false;
                }

                present_context clear{sizeof(clear), 1, 0, 0, 0, 0, 0, 0};
                if (source.observation.session && !smf_po_context(&clear)) return false;
                source.observation = {};
                source.begun = 0;
                source.composition.Reset();
                source.context.Reset();
                source.device.Reset();
                source.poisoned = false;
            }

            result.evidence = evidence_render_owners_retired;
            return true;
        }

        bool pump_operation(operation& op, outcome& result) {
            auto& s = state();
            const auto c = op.command;
            result.command = c;

            if (c.operation < op_restore_native && c.serial < s.operation_fence.load()) {
                result.superseded = true;
                result.hr = S_OK;
                return true;
            }

            const bool content_bound = c.operation == op_prepare_hidden || c.operation == op_prepare_replacement || c.operation == op_activate;
            if (content_bound && c.content_revision != s.content.load()) {
                result.superseded = true;
                return true;
            }

            size_t index = generation_count;
            for (size_t i = 0; i < generation_count; ++i) {
                if (s.status.generations[i].generation == c.generation) index = i;
            }

            switch (c.operation) {
            case op_prepare_hidden:
            case op_prepare_replacement:
                return pump_prepare(op, index, result);

            case op_invalidate_world:
                if (s.status.content_acknowledged >= c.content_revision) {
                    result.evidence = evidence_world_invalidated;
                    result.frame = s.status.active_frame;
                    return true;
                }
                return false;

            case op_activate:
                return pump_activate(op, index, result);

            case op_restore_native:
                result.evidence = evidence_operation_fence_accepted | (s.ever_active ? 0u : evidence_never_activated);
                result.frame = c.after_frame;
                return true;

            case op_await_native_frame:
                if (native_handoff_ready(s.status, s.operation_fence.load(), std::max(c.after_frame, s.restore_after_frame.load()))) {
                    result.evidence = evidence_native_frame_available | evidence_source_render_ordered | evidence_present_submitted;
                    result.frame = s.status.native_submitted_frame;
                    result.commit = s.status.native_present_serial;
                    return true;
                }
                return false;

            case op_detach:
                if (!s.ever_active || native_handoff_ready(s.status, s.operation_fence.load(), s.restore_after_frame.load())) {
                    s.detach_requested = c.serial;
                    wake();
                    if (s.detach_completed == c.serial) {
                        result.evidence = evidence_composite_detached | evidence_commit_processed;
                        result.frame = s.status.native_submitted_frame;
                        return true;
                    }
                }
                return false;

            case op_retire_generation:
            case op_retire_session:
                return pump_retire(c, result);

            case op_stop_worker: {
                bool retired = true;
                for (const auto& status : s.status.generations) {
                    if (status.state && status.state != 5) retired = false;
                }
                if (retired && s.detach_requested && s.detach_completed == s.detach_requested && completion_idle()) {
                    s.stop_requested = true;
                    wake();
                }
                return false;
            }

            default:
                return false;
            }
        }

    }

    void source_callback(ticket& t) {
        auto& s = state();
        lock held(s.gate);

        if (t.kind == ticket_kind::native_frame) {
            native_marker(t.native);
            return;
        }

        const bool pre_gui = t.kind == ticket_kind::pre_gui;
        const uint64_t session = pre_gui ? t.pre.session : t.frame.session;
        const uint64_t content = pre_gui ? t.pre.content_revision : t.frame.content_revision;
        const uint64_t generation_id = pre_gui ? t.pre.generation : t.frame.generation;
        if (!is_current(session, content, generation_id)) {
            ++s.status.dropped_frames;
            return;
        }

        size_t index = generation_count;
        for (size_t i = 0; i < generation_count; ++i) {
            if (s.status.generations[i].generation == generation_id) index = i;
        }
        if (index == generation_count) return;

        auto& status = s.status.generations[index];
        auto& g = source.slots[index];
        if (g.poisoned || source.poisoned) return;

        if (pre_gui) {
            if (t.pre.width != status.width || t.pre.height != status.height || t.pre.flags != status.flags) return;
            if (t.pre.source_frame <= status.last_source_frame) return;
            const HRESULT hr = stage_pre_gui(g, status, t);
            if (FAILED(hr)) record_failure(index, hr);
            return;
        }

        const auto& f = t.frame;
        if (g.staged_frame != f.source_frame || !g.staging || f.source_frame <= status.last_source_frame) return;
        if ((f.flags & session_has_map) != status.flags) return;
        present_frame(index, g, status, t, generation_id, content);
    }

    void source_pump() {
        auto& s = state();
        std::array<outcome, operation_count> results{};
        size_t count = 0;

        {
            lock held(s.gate);
            poll_native();
            pump_scenes();
            pump_generations();

            for (auto& op : s.operations) {
                if (!op.used) continue;
                outcome result{};
                bool done = pump_operation(op, result);

                // A superseded activation may still own a completion request; drain it first.
                if (done && result.superseded && op.command.operation == op_activate && op.phase == 2) {
                    if (completion_poll(op.completion) == S_FALSE) done = false;
                }
                if (done) results[count++] = result;
            }
        }

        for (size_t i = 0; i < count; ++i) {
            const auto& r = results[i];
            acknowledge(r.command, r.hr, r.evidence, r.frame, r.commit, r.superseded);
        }
    }

}
