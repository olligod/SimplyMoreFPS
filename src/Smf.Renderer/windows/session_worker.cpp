#include "session_internal.h"
#include "present_observer.h"
#include "scene_transport.h"
#include "../common/selection_overlay.h"
#include <algorithm>
#include <cstring>
#include <new>

// The compositor worker: a dedicated native thread that owns the DirectComposition
// target on the Unity window and keeps the camera moving while Unity is blocked.
namespace session {
    namespace {

        struct worker_slot {
            ComPtr<IDCompositionVisual> background_parent;
            ComPtr<IDCompositionVisual> map_parent;
            ComPtr<IDCompositionVisual> hud_parent;
            // Borrowed from the source; AddVisual holds them, the source releases after removal.
            IDCompositionVisual* background = nullptr;
            IDCompositionVisual* map = nullptr;
            IDCompositionVisual* hud = nullptr;
            uint64_t requested = 0;
            uint64_t completion = 0;
            uint64_t commit = 0;
            D2D_MATRIX_3X2_F displayed{1, 0, 0, 1, 0, 0};
        };

        struct compositor {
            ComPtr<IDCompositionDevice> composition;
            ComPtr<IDCompositionTarget> target;
            ComPtr<IDCompositionVisual> root;
            ComPtr<ID3D11Device> selection_device;
            ComPtr<ID3D11DeviceContext> selection_context;
            ComPtr<IDCompositionSurface> selection_surface;
            std::array<ComPtr<IDCompositionVisual>, 4> selection_visuals{};
            selection::worker overlay;
            selection::geometry overlay_displayed{};
            std::array<worker_slot, generation_count> slots{};
            std::array<std::shared_ptr<scene_channel>, generation_count> scene_channels{};
            std::array<std::unique_ptr<scene_worker>, generation_count> scene_renderers{};
            std::array<smf_bridge_desired, generation_count> scene_desired{};
            std::array<bool, generation_count> scene_requested{};
            std::array<bool, generation_count> scene_pose_valid{};
            uint64_t detach_commit = 0;
            uint64_t detach_completion = 0;

            HRESULT create() {
                // A private device that only paints the one-pixel selection brush. Unity's
                // device and textures stay on Unity's render thread.
                HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    nullptr, 0, D3D11_SDK_VERSION, &selection_device, nullptr, &selection_context);

                ComPtr<IDXGIDevice> dxgi;
                if (SUCCEEDED(hr)) hr = selection_device.As(&dxgi);
                if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition));
                if (SUCCEEDED(hr)) hr = composition->CreateTargetForHwnd(state().window, FALSE, &target);
                if (SUCCEEDED(hr)) hr = composition->CreateVisual(&root);

                for (auto& slot : slots) {
                    if (SUCCEEDED(hr)) hr = composition->CreateVisual(&slot.background_parent);
                    if (SUCCEEDED(hr)) hr = composition->CreateVisual(&slot.map_parent);
                    if (SUCCEEDED(hr)) hr = composition->CreateVisual(&slot.hud_parent);
                    if (SUCCEEDED(hr)) hr = root->AddVisual(slot.background_parent.Get(), FALSE, nullptr);
                }

                // Z order, bottom up: backgrounds, maps, selection edges, HUDs. AddVisual with a
                // null reference and FALSE places the visual above all existing siblings.
                for (auto& slot : slots) {
                    if (SUCCEEDED(hr)) hr = root->AddVisual(slot.map_parent.Get(), FALSE, nullptr);
                }

                if (SUCCEEDED(hr)) {
                    hr = composition->CreateSurface(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &selection_surface);
                }
                for (auto& visual : selection_visuals) {
                    if (SUCCEEDED(hr)) hr = composition->CreateVisual(&visual);
                    if (SUCCEEDED(hr)) hr = visual->SetContent(selection_surface.Get());
                    if (SUCCEEDED(hr)) hr = visual->SetClip(D2D_RECT_F{0, 0, 0, 0});
                    if (SUCCEEDED(hr)) hr = root->AddVisual(visual.Get(), FALSE, nullptr);
                }

                for (auto& slot : slots) {
                    if (SUCCEEDED(hr)) hr = root->AddVisual(slot.hud_parent.Get(), FALSE, nullptr);
                }

                if (SUCCEEDED(hr)) hr = target->SetRoot(root.Get());
                if (SUCCEEDED(hr)) hr = composition->Commit();
                if (SUCCEEDED(hr)) ++state().status.worker_commit;
                return hr;
            }

            HRESULT commit(uint64_t& serial) {
                const HRESULT hr = composition->Commit();
                if (hr == S_OK) serial = ++state().status.worker_commit;
                return hr;
            }

            void fail(HRESULT hr) {
                state().status.result = hr;
                state().status.worker_state = 4;
            }

            HRESULT set_selection(const selection::geometry& next, bool& changed) {
                changed = next.visible != overlay_displayed.visible;
                bool color_changed = false;

                if (next.visible) {
                    for (size_t i = 0; i < 4; ++i) {
                        const auto& a = next.edges[i];
                        const auto& b = overlay_displayed.edges[i];
                        changed |= a.left != b.left || a.top != b.top || a.right != b.right || a.bottom != b.bottom;
                        color_changed |= next.color[i] != overlay_displayed.color[i];
                    }
                }

                changed |= color_changed;
                if (!changed) return S_OK;

                HRESULT hr = S_OK;
                if (next.visible && (color_changed || !overlay_displayed.visible)) {
                    POINT offset{};
                    ComPtr<ID3D11Texture2D> texture;

                    hr = selection_surface->BeginDraw(nullptr, IID_PPV_ARGS(&texture), &offset);
                    if (SUCCEEDED(hr)) {
                        const auto channel = [](float value) { return static_cast<uint8_t>(value * 255 + .5f); };
                        const uint8_t color[]{channel(next.color[2] * next.color[3]), channel(next.color[1] * next.color[3]),
                            channel(next.color[0] * next.color[3]), channel(next.color[3])};
                        const D3D11_BOX box{static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
                            static_cast<UINT>(offset.x + 1), static_cast<UINT>(offset.y + 1), 1};

                        selection_context->UpdateSubresource(texture.Get(), 0, &box, color, 4, 4);
                        const HRESULT end = selection_surface->EndDraw();
                        if (SUCCEEDED(hr)) hr = end;
                    }
                }

                for (size_t i = 0; i < 4 && SUCCEEDED(hr); ++i) {
                    const auto& edge = next.edges[i];
                    hr = selection_visuals[i]->SetClip(next.visible ? D2D_RECT_F{0, 0, 1, 1} : D2D_RECT_F{0, 0, 0, 0});
                    if (SUCCEEDED(hr) && next.visible) {
                        hr = selection_visuals[i]->SetTransform(D2D_MATRIX_3X2_F{edge.right - edge.left, 0, 0, edge.bottom - edge.top, edge.left, edge.top});
                    }
                }

                if (SUCCEEDED(hr)) overlay_displayed = next;
                return hr;
            }

            void hide_selection() {
                bool changed = false;
                HRESULT hr = set_selection({}, changed);
                uint64_t serial = 0;
                if (SUCCEEDED(hr) && changed) hr = commit(serial);
                if (FAILED(hr)) fail(hr);
            }

            // Swaps source visuals in and out of the parents as the links change.
            void pump_links() {
                auto& s = state();

                for (size_t i = 0; i < generation_count; ++i) {
                    auto& slot = slots[i];
                    auto& l = s.links[i];

                    if (slot.completion) {
                        uint64_t completed = 0;
                        const HRESULT hr = completion_poll(slot.completion, &completed);
                        if (hr != S_FALSE) {
                            slot.completion = 0;
                            if (FAILED(hr)) {
                                fail(hr);
                                return;
                            }

                            s.status.worker_completed = std::max(s.status.worker_completed, completed);
                            if (l.serial == slot.requested) {
                                l.acknowledged = slot.requested;
                                l.attached = visual_bundle_complete(slot.background != nullptr, slot.map != nullptr, slot.hud != nullptr);
                            }

                            slot.requested = 0;
                            slot.commit = 0;
                        }
                    }

                    if (slot.commit && !slot.completion) slot.completion = completion_request(composition.Get(), slot.commit, s.session.load());
                    if (slot.requested || l.acknowledged == l.serial || !l.serial) continue;

                    HRESULT hr = S_OK;
                    if (slot.background) hr = slot.background_parent->RemoveVisual(slot.background);
                    if (SUCCEEDED(hr) && slot.map) hr = slot.map_parent->RemoveVisual(slot.map);
                    if (SUCCEEDED(hr) && slot.hud) hr = slot.hud_parent->RemoveVisual(slot.hud);
                    if (SUCCEEDED(hr) && l.background) hr = slot.background_parent->AddVisual(l.background, FALSE, nullptr);
                    if (SUCCEEDED(hr) && l.map) hr = slot.map_parent->AddVisual(l.map, FALSE, nullptr);
                    if (SUCCEEDED(hr) && l.hud) hr = slot.hud_parent->AddVisual(l.hud, FALSE, nullptr);

                    // A new generation starts at its reference pose.
                    const D2D_MATRIX_3X2_F identity{1, 0, 0, 1, 0, 0};
                    if (SUCCEEDED(hr)) hr = slot.map_parent->SetTransform(identity);
                    if (SUCCEEDED(hr)) hr = commit(slot.commit);
                    if (FAILED(hr)) {
                        fail(hr);
                        return;
                    }

                    slot.background = l.background;
                    slot.map = l.map;
                    slot.hud = l.hud;
                    slot.displayed = identity;
                    slot.requested = l.serial;
                    slot.completion = completion_request(composition.Get(), slot.commit, s.session.load());
                }
            }

            void camera() {
                auto& s = state();
                scene_requested = {};
                const uint64_t fence = s.content.load();

                // The gate orders the fence against source and parent commits. Acknowledging it
                // freezes the last complete bundle; historical UI is kept visible, not blanked.
                if (s.status.content_acknowledged < fence) {
                    s.status.content_acknowledged = fence;
                    if (retain_historical(s.status, fence)) s.status.flags |= 4u;
                }

                if (s.captures_sealed.load()) {
                    hide_selection();
                    return;
                }

                size_t index = generation_count;
                for (size_t i = 0; i < generation_count; ++i) {
                    const auto& g = s.status.generations[i];
                    if (g.generation == s.status.active_generation && g.content_revision == fence && g.state == 3 && s.models[i].valid) index = i;
                }

                if (index == generation_count) {
                    hide_selection();
                    return;
                }

                auto& slot = slots[index];
                const auto& m = s.models[index];
                if (!s.links[index].attached || slot.requested) {
                    hide_selection();
                    return;
                }

                const smf_selection_state overlay_snapshot = overlay.latch();
                POINT point{};
                const bool point_valid = GetCursorPos(&point) && ScreenToClient(s.window, &point);
                const bool focused = GetForegroundWindow() == s.window;
                const bool middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
                smf_bridge_desired desired{};
                const HRESULT prepared = camera_bridge::worker_prepare(focused, point_valid, point.x, point.y, middle, desired);
                if (prepared != S_OK || desired.epoch != m.camera_epoch || desired.map_id != m.map_id) {
                    hide_selection();
                    return;
                }

                affine desired_projection{};
                D2D_MATRIX_3X2_F transform{};
                if (!root_projection(m, desired.x, desired.z, desired.projection_half_height, desired_projection) ||
                    !mapping(desired_projection, m.nominal, m.height, false, transform)) {
                    hide_selection();
                    return;
                }

                const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                const selection::geometry outline = overlay.read(overlay_snapshot, s.session.load(), fence, m.map_id, desired_projection,
                    focused, point_valid, left, point.x, point.y);
                bool outline_changed = false;
                HRESULT hr = set_selection(outline, outline_changed);
                const bool scene = s.scenes[index] != nullptr;
                if (scene) {
                    scene_requested[index] = true;
                    scene_pose_valid[index] = true;
                    scene_desired[index] = desired;
                }
                const bool camera_changed = !scene && std::memcmp(&transform, &slot.displayed, sizeof(transform)) != 0;

                if (SUCCEEDED(hr) && !camera_changed && !outline_changed) {
                    // Same pose as the last Commit: publish it without another ~1000 commits/s.
                    if (!scene) camera_bridge::worker_reused_pose();
                    return;
                }

                if (SUCCEEDED(hr) && camera_changed) hr = slot.map_parent->SetTransform(transform);
                uint64_t serial = 0;
                if (SUCCEEDED(hr)) hr = commit(serial);
                if (!scene) camera_bridge::worker_committed(hr);
                if (FAILED(hr)) {
                    fail(hr);
                    return;
                }

                slot.displayed = transform;
            }

            // Compilation, shared-resource admission and scene rendering never hold the session gate.
            void scenes() {
                auto& s = state();
                for (size_t i = 0; i < generation_count; ++i) {
                    std::shared_ptr<scene_channel> channel;
                    session_generation_status generation{};
                    smf_bridge_desired desired{};
                    bool requested = false;
                    bool allowed = false;
                    {
                        lock held(s.gate);
                        channel = s.scenes[i];
                        generation = s.status.generations[i];
                        requested = scene_requested[i];
                        desired = scene_desired[i];
                        allowed = s.status.worker_state == 2 && is_current(s.session.load(),
                            generation.content_revision, generation.generation);
                    }
                    if (!channel) {
                        scene_renderers[i].reset();
                        scene_channels[i].reset();
                        scene_pose_valid[i] = false;
                        continue;
                    }
                    if (scene_channels[i] != channel || !scene_renderers[i]) {
                        scene_renderers[i].reset(new (std::nothrow) scene_worker());
                        if (!scene_renderers[i]) {
                            if (channel->retiring.load()) {
                                channel->worker_retired = true;
                                continue;
                            }
                            lock held(s.gate);
                            fail(E_OUTOFMEMORY);
                            continue;
                        }
                        scene_channels[i] = channel;
                        scene_pose_valid[i] = false;
                    }
                    auto& renderer = *scene_renderers[i];
                    HRESULT hr = S_FALSE;
                    if (channel->retiring.load()) {
                        hr = renderer.retire(*channel);
                    } else if (!allowed) {
                        hr = renderer.drain(*channel);
                    } else if (allowed && (generation.state != 3 || scene_pose_valid[i])) {
                        hr = renderer.prepare(*channel, generation.state == 3 ? &desired : nullptr);
                        if (SUCCEEDED(hr)) {
                            lock held(s.gate);
                            if (s.scenes[i] == channel && s.status.worker_state == 2 &&
                                is_current(s.session.load(), generation.content_revision, generation.generation)) {
                                hr = renderer.present(*channel);
                                if (hr == S_OK) {
                                    ++s.status.worker_commit;
                                    if (generation.state == 3) {
                                        s.status.active_frame = renderer.source_frame();
                                        camera_bridge::worker_committed(hr);
                                    }
                                    wake();
                                } else if (requested && renderer.reused_pose()) {
                                    camera_bridge::worker_reused_pose();
                                }
                            }
                        }
                    }
                    if (FAILED(hr)) {
                        lock held(s.gate);
                        if (s.scenes[i] == channel) fail(hr);
                    }
                }
            }

            void detach() {
                auto& s = state();
                if (!s.detach_requested || s.detach_completed == s.detach_requested) return;

                if (!target || !composition) {
                    if (!s.ever_active) {
                        s.detach_completed = s.detach_requested;
                        s.status.flags &= ~1u;
                    }
                    return;
                }

                if (!detach_commit) {
                    HRESULT hr = target->SetRoot(nullptr);
                    if (SUCCEEDED(hr)) hr = commit(detach_commit);
                    if (FAILED(hr)) {
                        fail(hr);
                        return;
                    }
                }

                if (!detach_completion) detach_completion = completion_request(composition.Get(), detach_commit, s.session.load());
                if (!detach_completion) return;

                uint64_t completed = 0;
                const HRESULT hr = completion_poll(detach_completion, &completed);
                if (hr == S_FALSE) return;
                detach_completion = 0;
                if (FAILED(hr)) {
                    fail(hr);
                    return;
                }

                s.status.worker_completed = std::max(s.status.worker_completed, completed);
                s.detach_completed = s.detach_requested;
                s.status.flags &= ~1u;
            }
        };

    }

    DWORD WINAPI worker_run(void*) {
        auto& s = state();
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT wheel = FAILED(initialized) ? initialized : camera_control::start_wheel(s.window);
        compositor worker;

        {
            lock l(s.gate);
            s.status.worker_thread = GetCurrentThreadId();
            HRESULT hr;
            if (wheel == S_OK) {
                hr = worker.create();
            } else {
                hr = FAILED(wheel) ? wheel : HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            s.status.result = hr;
            s.status.worker_state = SUCCEEDED(hr) ? 2u : 4u;
            if (SUCCEEDED(hr)) s.status.flags |= 1u;
        }

        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        camera_bridge::worker_ready(GetCurrentThreadId(), frequency.QuadPart);

        for (;;) {
            {
                lock l(s.gate);
                if (s.stop_requested.load()) break;

                if (s.status.worker_state == 2) {
                    if (!owned_window()) {
                        worker.fail(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
                    } else {
                        worker.pump_links();
                        worker.camera();
                        worker.detach();
                    }
                } else if (s.status.worker_state == 4 && s.detach_requested) {
                    // After a failure the picture is frozen, but an explicit off still detaches.
                    if (worker.composition && worker.root) {
                        worker.pump_links();
                    } else if (!s.ever_active) {
                        for (auto& entry : s.links) {
                            entry.acknowledged = entry.serial;
                            entry.attached = false;
                        }
                    }

                    worker.detach();
                }
            }

            worker.scenes();

            // Wake on commands and completions; otherwise a bounded cadence, never a spin.
            WaitForSingleObject(s.wake, 1);
        }

        camera_bridge::worker_removed();

        HRESULT wheel_stop;
        while ((wheel_stop = camera_control::stop_wheel(0)) == S_FALSE) {
            WaitForSingleObject(s.wake, 1);
        }

        // Release all COM objects on this thread before signalling that it has stopped.
        worker.slots = {};
        worker.scene_renderers = {};
        worker.scene_channels = {};
        worker.root.Reset();
        worker.target.Reset();
        worker.selection_visuals = {};
        worker.selection_surface.Reset();
        worker.composition.Reset();
        worker.selection_context.Reset();
        worker.selection_device.Reset();

        completion_request_stop();
        {
            lock l(s.gate);
            s.stop_result = wheel_stop;
            s.status.worker_state = 3;
        }

        if (SUCCEEDED(initialized)) CoUninitialize();
        return 0;
    }

    HRESULT worker_poll_joined() {
        auto& s = state();
        session_command stop{};
        HRESULT stopped = S_OK;

        {
            try_lock l(s.gate);
            if (!l) return S_FALSE;

            if (s.worker) {
                const DWORD waited = WaitForSingleObject(s.worker, 0);
                if (waited == WAIT_TIMEOUT) return S_FALSE;
                if (waited != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());

                CloseHandle(s.worker);
                s.worker = nullptr;
                CloseHandle(s.wake);
                s.wake = nullptr;
                s.status.worker_state = 5;
            }

            const HRESULT completion = completion_poll_joined();
            if (completion != S_OK) return completion;
            for (const auto& op : s.operations) {
                if (op.used && op.command.operation == op_stop_worker) stop = op.command;
            }
            stopped = s.stop_result;
        }

        if (stop.serial) acknowledge(stop, stopped, SUCCEEDED(stopped) ? evidence_worker_joined : 0);
        return S_OK;
    }

}
