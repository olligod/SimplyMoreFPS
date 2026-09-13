#pragma once
#import <Metal/Metal.h>
#include "../common/scene_packets.h"
#include <memory>

namespace mac {

    struct scene_frame {
        const smf_scene::description* scene = nullptr;
        const smf_scene::image* images = nullptr;
        const smf_scene::layer* layers = nullptr;
        const smf_scene::effect* effects = nullptr;
        id<MTLTexture> const* resources = nullptr;
    };

    struct scene_view {
        uint32_t width = 0, height = 0;
        double map_affine[6]{};
        double camera_x = 0, camera_z = 0;
    };

    class scene_compositor {
    public:
        scene_compositor();
        ~scene_compositor();
        scene_compositor(const scene_compositor&) = delete;
        scene_compositor& operator=(const scene_compositor&) = delete;

        int initialize(id<MTLDevice> device);
        int draw(id<MTLCommandBuffer> command, const scene_frame& source, const scene_view& desired);
        id<MTLTexture> texture() const;
        void release();

    private:
        struct state;
        std::unique_ptr<state> state_;
    };

}
