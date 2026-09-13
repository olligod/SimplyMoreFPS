#pragma once
#include "../common/scene_packets.h"
#include <memory>
#include <d3d11.h>

namespace smf_scene {

    struct frame {
        const description* scene = nullptr;
        const image* images = nullptr;
        const layer* layers = nullptr;
        const effect* effects = nullptr;
        ID3D11ShaderResourceView* const* resources = nullptr;
    };

    struct view {
        uint32_t width = 0, height = 0;
        double map_affine[6]{};
        double camera_x = 0, camera_z = 0;
    };

    class image_filter_executor {
    public:
        virtual ~image_filter_executor() = default;
        virtual bool supports(uint64_t program, uint32_t pass) const = 0;
        virtual HRESULT execute(ID3D11DeviceContext* context, const frame& source, const effect& operation,
            ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, uint32_t width, uint32_t height) = 0;
    };

    class scene_compositor {
    public:
        scene_compositor();
        ~scene_compositor();
        scene_compositor(const scene_compositor&) = delete;
        scene_compositor& operator=(const scene_compositor&) = delete;

        HRESULT initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        HRESULT draw(const frame& source, const view& desired);
        void set_filter_executor(image_filter_executor* executor);
        ID3D11Texture2D* texture() const;
        ID3D11ShaderResourceView* shader_resource() const;

        // The owner waits for its last submission before releasing or resizing this component.
        void release();

    private:
        struct state;
        std::unique_ptr<state> state_;
    };

}
