#pragma once
#include "session_internal.h"
#include "scene_compositor.h"
#include "../common/scene_snapshot.h"
#include <memory>
#include <vector>

namespace session {

    struct scene_image {
        ~scene_image();
        ComPtr<ID3D11Texture2D> original;
        ComPtr<ID3D11Texture2D> source;
        ComPtr<ID3D11Texture2D> worker;
        ComPtr<ID3D11ShaderResourceView> view;
        HANDLE handle = nullptr;
        smf_scene::image description{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t allocation_bytes = 0;
    };

    enum class scene_phase { free, copying, ready, reading };

    struct scene_bundle {
        std::atomic<scene_phase> phase{scene_phase::free};
        smf_scene::snapshot snapshot{};
        session_frame frame{};
        model camera{};
        std::array<std::shared_ptr<scene_image>, smf_scene::maximum_images + 2> images{};
        ComPtr<ID3D11Query> source_completion;
        ComPtr<ID3D11Query> worker_completion;
    };

    class scene_channel {
    public:
        static constexpr size_t capacity = 3;

        HRESULT initialize(ID3D11Device*, ID3D11DeviceContext*);
        HRESULT submit(const smf_scene::snapshot&, const session_frame&, const model&);
        HRESULT poll(bool discard = false);
        void discard_ready();
        void trim();
        scene_bundle* acquire();
        bool idle() const;

        std::atomic<bool> retiring{false};
        std::atomic<bool> worker_retired{false};
        std::atomic<uint64_t> presented_frame{0};
        std::atomic<uint64_t> prepared_frame{0};
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDCompositionVisual> background;
        ComPtr<IDCompositionVisual> world;
        ComPtr<IDCompositionVisual> hud;

    private:
        HRESULT image_cost(const smf_scene::image&, DXGI_FORMAT&, uint64_t&) const;
        HRESULT copy_cost(const smf_scene::snapshot&, const session_frame&, const model&, uint64_t&) const;
        HRESULT copy_image(const smf_scene::image&, std::shared_ptr<scene_image>&, uint64_t&);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        std::array<scene_bundle, capacity> bundles{};
        std::vector<std::shared_ptr<scene_image>> pool;
    };

    class scene_worker {
    public:
        scene_worker();
        ~scene_worker();
        scene_worker(const scene_worker&) = delete;
        scene_worker& operator=(const scene_worker&) = delete;

        HRESULT prepare(scene_channel&, const smf_bridge_desired*);
        HRESULT present(scene_channel&);
        HRESULT drain(scene_channel&);
        HRESULT retire(scene_channel&);
        bool prepared() const;
        bool changed() const;
        bool reused_pose() const;
        uint64_t source_frame() const;

    private:
        struct implementation;
        std::unique_ptr<implementation> state;
    };

}
