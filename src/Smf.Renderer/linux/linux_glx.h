#pragma once
#define GL_GLEXT_PROTOTYPES
#include "linux_core.h"
#include "camera_tuple.h"
#include "capture_diagnostic.h"
#include "display_ownership.h"
#include "geometry_policy.h"
#include "performance_status.h"
#include "x_error_ledger.h"
#include "../common/selection_overlay.h"
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <cstdint>
#include <memory>
#include <string>

namespace linux_session {

    // The GL bindings a source copy touches, saved before and restored after.
    struct gl_state {
        GLint read_fbo = 0;
        GLint draw_fbo = 0;
        GLint read_buffer = 0;
        GLint draw_buffer = 0;
        GLint active = 0;
        GLint texture = 0;
        GLboolean scissor = 0;
        GLboolean srgb = 0;

        gl_state();
        bool restore() const;
    };

    // Unity's own GLX context, used only from inside its end-of-frame callback.
    struct source_glx {
        Display* display = nullptr;
        GLXContext context = nullptr;
        GLXDrawable drawable = 0;
        int config = 0;
        int screen = 0;
        int major = 0;
        int minor = 0;
        int profile = 0;
        uint32_t thread = 0;
        std::string display_name;
        std::string renderer;
        std::shared_ptr<x_error_ledger> errors;
        GLuint read_fbo = 0;
        GLuint draw_fbo = 0;
        copy_diagnostic last_copy{};
        uint64_t performance[16]{}; // written by the source callback, published under the session gate

        bool discover(uint32_t width, uint32_t height);
        bool own() const;
        bool copy(GLuint source, GLuint& owned, uint32_t width, uint32_t height, bool original = false);
        void release();
    };

    struct layer {
        GLuint texture = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        affine source{};
        bool flip = false;
    };

    // An offscreen child window Unity is routed to while the worker owns the original.
    struct hidden_surface {
        Window window = 0;
        GLXWindow drawable = 0;
        Colormap colormap = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        GLsync source_fence = nullptr;
        bool source_departed = false;
        bool delete_submitted = false;
    };

    // The worker thread's own GLX context, sharing objects with the source context.
    struct worker_glx {
        selection::worker overlay;
        Display* display = nullptr; // borrowed from Unity, never closed here
        Display* input_display = nullptr; // owned XInput-only connection, never creates GL
        Window original = 0;
        Window child = 0;
        GLXWindow drawable = 0;
        GLXContext context = nullptr;
        Colormap colormap = 0;
        GLuint program = 0;
        GLuint vao = 0;
        GLuint sampler = 0;
        std::shared_ptr<x_error_ledger> error_scope;
        uint32_t width = 0;
        uint32_t height = 0;
        // Read while this context is current; diagnostics never substitute the source strings.
        std::string renderer;
        std::string vendor;
        std::string version;
        int config = 0;
        int screen = 0;
        uint64_t draw_facts[8]{}; // worker-owned, copied after draw only
        GLsync object_retirement_fence = nullptr;
        bool object_deletes_issued = false;
        bool objects_retired = false;

        bool create(const source_glx&, Window, uint32_t, uint32_t);
        geometry_outcome geometry(geometry_ticket, uint64_t* facts);
        bool healthy();
        draw_outcome draw(geometry_ticket ticket, const layer& base, const layer& world, const layer& hud, const layer& cache,
                          const affine& desired, uint32_t logical_width, uint32_t logical_height,
                          const selection::geometry& selection_geometry);
        hidden_surface take_initial_hidden();
        geometry_outcome create_hidden(uint32_t, uint32_t, hidden_surface&);
        bool destroy_hidden(hidden_surface&);
        int retire_objects(); // context current: -1 failure, 0 pending, 1 done
        void close_input(); // worker only, after the input observer is cleared
        bool destroy(); // false keeps whatever could not be cleaned up
    };

}
