#pragma once
#define GL_GLEXT_PROTOTYPES
#include "../common/scene_snapshot.h"
#include <GL/gl.h>
#include <array>
#include <memory>

namespace smf_scene {

    struct frame {
        const snapshot* scene = nullptr;
        const GLuint* resources = nullptr;
    };

    struct view {
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
        scene_compositor(scene_compositor&&) noexcept;
        scene_compositor& operator=(scene_compositor&&) noexcept;

        bool initialize();
        bool draw(const frame& source, const view& desired);
        GLuint texture() const;
        void clear_textures();
        void release();

    private:
        struct state;
        std::unique_ptr<state> state_;
    };

}
