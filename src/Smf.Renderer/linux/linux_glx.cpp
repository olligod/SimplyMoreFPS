#include "linux_glx.h"
#include "clock.h"
#include <X11/Xlibint.h>
#undef min
#undef max
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <GL/glxext.h>
#include <algorithm>
#include <cstring>

namespace linux_session {
    namespace {

        bool locking(Display* d) {
            return d && d->lock && d->lock_fns && d->lock_fns->lock_display && d->lock_fns->unlock_display;
        }

        bool good(worker_glx& w) {
            return w.error_scope && !w.error_scope->read().count;
        }

        bool own_window(Display* d, Window w) {
            Atom property = XInternAtom(d, "_NET_WM_PID", True);
            if (!property) return false;

            Atom type = 0;
            int format = 0;
            unsigned long count = 0;
            unsigned long left = 0;
            unsigned char* data = nullptr;

            int r = XGetWindowProperty(d, w, property, 0, 1, False, XA_CARDINAL, &type, &format, &count, &left, &data);
            bool okay = r == Success && type == XA_CARDINAL && format == 32 && count == 1 && data &&
                        *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(getpid());

            if (data) XFree(data);
            return okay;
        }

        GLuint compile_shader(GLenum stage, const char* source) {
            GLuint s = glCreateShader(stage);
            glShaderSource(s, 1, &source, nullptr);
            glCompileShader(s);

            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                glDeleteShader(s);
                return 0;
            }

            return s;
        }

        const char* vertex_source = R"(#version 420 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2. - 1., 0., 1.);
}
)";

        const char* fragment_source = R"(#version 420 core
layout(location = 0) out vec4 color;
uniform sampler2D raster;
uniform vec2 output_size;
uniform vec2 raster_size;
uniform vec2 pixel_scale;
uniform vec3 map_x;
uniform vec3 map_y;
uniform int row_flip;
uniform int opaque;
uniform int solid;
uniform vec4 solid_color;

void main() {
    if (solid != 0) {
        color = solid_color;
        return;
    }
    vec3 screen = vec3(gl_FragCoord.x * pixel_scale.x, output_size.y - gl_FragCoord.y * pixel_scale.y, 1.);
    vec2 p = vec2(dot(map_x, screen), dot(map_y, screen));
    if (any(lessThan(p, vec2(0.))) || any(greaterThanEqual(p, raster_size))) discard;
    vec2 uv = p / raster_size;
    if (row_flip == 0) uv.y = 1. - uv.y;
    color = texture(raster, uv);
    if (opaque != 0) color.a = 1.;
}
)";

    }

    gl_state::gl_state() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    }

    bool gl_state::restore() const {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glReadBuffer(read_buffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glActiveTexture(active);
        glBindTexture(GL_TEXTURE_2D, texture);

        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        if (srgb) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        } else {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }

        gl_state after;
        return read_fbo == after.read_fbo && draw_fbo == after.draw_fbo && read_buffer == after.read_buffer &&
               draw_buffer == after.draw_buffer && active == after.active && texture == after.texture &&
               scissor == after.scissor && srgb == after.srgb;
    }

    bool source_glx::discover(uint32_t width, uint32_t height) {
        display = glXGetCurrentDisplay();
        context = glXGetCurrentContext();
        drawable = glXGetCurrentDrawable();
        thread = native_thread();

        if (!display || !context || !drawable || glXGetCurrentReadDrawable() != drawable || !locking(display) ||
            !glXIsDirect(display, context)) return false;
        if (glXQueryContext(display, context, GLX_FBCONFIG_ID, &config) ||
            glXQueryContext(display, context, GLX_SCREEN, &screen)) return false;

        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
        if (major < 4 || (major == 4 && minor < 2) || profile != GL_CONTEXT_CORE_PROFILE_BIT) return false;

        unsigned w = 0, h = 0;
        glXQueryDrawable(display, drawable, GLX_WIDTH, &w);
        glXQueryDrawable(display, drawable, GLX_HEIGHT, &h);
        if (w != width || h != height) return false;

        display_name = DisplayString(display);
        renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        glGenFramebuffers(1, &read_fbo);
        glGenFramebuffers(1, &draw_fbo);
        return read_fbo && draw_fbo;
    }

    bool source_glx::own() const {
        return native_thread() == thread && glXGetCurrentDisplay() == display && glXGetCurrentContext() == context &&
               glXGetCurrentDrawable() == drawable && glXGetCurrentReadDrawable() == drawable;
    }

    bool source_glx::copy(GLuint source, GLuint& owned, uint32_t width, uint32_t height, bool original,
                          GLenum copy_format) {
        last_copy = {};
        auto& d = last_copy.words;
        d[0] = 1;
        d[2] = source;
        d[3] = owned;
        d[4] = width;
        d[5] = height;
        d[30] = original ? 1 : 0;

        auto failure = [&](uint32_t stage) {
            performance[copy_failures]++;
            d[1] = stage;
            d[29] = glGetError();
            d[31] |= 16;
            return false;
        };

        auto record_state = [&](const gl_state& g, unsigned start) {
            d[start] = g.read_fbo;
            d[start + 1] = g.draw_fbo;
            d[start + 2] = g.read_buffer;
            d[start + 3] = g.draw_buffer;
            d[start + 4] = g.active;
            d[start + 5] = g.texture;
        };

        // Without the context there is no GL error to read either.
        if (!own() || !width || !height || width > 16384 || height > 16384) {
            performance[copy_failures]++;
            d[1] = 1;
            return false;
        }

        gl_state before;
        record_state(before, 15);
        d[31] |= 1;

        GLint default_read = 0;
        bool touched_default = false;
        bool okay = true;
        uint32_t failed_stage = 0;

        if (original) {
            unsigned w = 0, h = 0;
            glXQueryDrawable(display, drawable, GLX_WIDTH, &w);
            glXQueryDrawable(display, drawable, GLX_HEIGHT, &h);

            d[8] = w;
            d[9] = h;
            d[31] |= 2;
            if ((!before.draw_fbo && before.draw_buffer != GL_BACK) || w != width || h != height)
                return failure(2);

            // Unity can restore its logical target before rebinding the framebuffer.
            if (before.draw_fbo) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

            GLint samples = 0;
            GLint type = 0;
            GLint r = 0;
            GLint g = 0;
            GLint b = 0;

            glGetIntegerv(GL_SAMPLES, &samples);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);

            d[10] = type;
            d[12] = samples;
            d[13] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
            if (samples > 1 || type != GL_UNSIGNED_NORMALIZED || r != 8 || g != 8 || b != 8) {
                before.restore();
                return failure(3);
            }

            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glGetIntegerv(GL_READ_BUFFER, &default_read);
            glReadBuffer(GL_BACK);
            touched_default = true;
        } else {
            d[6] = source ? glIsTexture(source) : 0;
            d[31] |= 4;
            if (!d[6]) return failure(4);

            glBindTexture(GL_TEXTURE_2D, source);
            GLint w = 0, h = 0;
            GLint format = 0;
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            d[7] = bound;
            if (GLuint(bound) != source) {
                okay = false;
                failed_stage = 5;
            }

            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
            d[8] = w;
            d[9] = h;
            d[10] = format;
            d[31] |= 2;
            const bool compatible = copy_format == GL_RGBA8 ? format == GL_RGBA8 || format == GL_SRGB8_ALPHA8 : format == GLint(copy_format);
            if (w != int(width) || h != int(height) || !compatible) {
                okay = false;
                if (!failed_stage) failed_stage = 6;
            }

            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            d[11] = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (d[11] != GL_FRAMEBUFFER_COMPLETE) {
                okay = false;
                if (!failed_stage) failed_stage = 7;
            }
        }

        if (okay) {
            if (!owned) {
                glGenTextures(1, &owned);
                if (owned) {
                    performance[allocated_names]++;
                    performance[live_names]++;
                    if (performance[live_names] > performance[peak_names]) performance[peak_names] = performance[live_names];
                }

                glBindTexture(GL_TEXTURE_2D, owned);
                glTexStorage2D(GL_TEXTURE_2D, 1, copy_format, width, height);
                performance[storage_bytes_issued] += uint64_t(width) * height * (copy_format == GL_RGBA16F ? 8 : 4);
            }

            d[3] = owned;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, owned, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            d[14] = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            okay = d[14] == GL_FRAMEBUFFER_COMPLETE;
            if (!okay) failed_stage = 8;
            if (okay) {
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_FRAMEBUFFER_SRGB);
                glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                performance[blit_calls]++;
                performance[blit_bytes_issued] += uint64_t(width) * height * (copy_format == GL_RGBA16F ? 8 : 4);
            }

            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        }

        if (!original) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        }

        if (touched_default) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(default_read);
        }

        bool restored = before.restore();
        d[28] = restored;
        if (!restored) {
            gl_state after;
            record_state(after, 21);
            d[31] |= 8;
            if (!failed_stage) failed_stage = 9;
        }

        return restored && okay ? true : failure(failed_stage);
    }

    void source_glx::release() {
        if (!own()) return;

        if (read_fbo) glDeleteFramebuffers(1, &read_fbo);
        if (draw_fbo) glDeleteFramebuffers(1, &draw_fbo);
        read_fbo = 0;
        draw_fbo = 0;
        errors.reset();
    }

    bool worker_glx::create(const source_glx& s, Window xid, uint32_t w, uint32_t h) {
        original = xid;
        width = w;
        height = h;
        config = s.config;
        screen = s.screen;
        display = s.display;

        if (!locking(display) || !s.errors || s.errors->connection() != display) return false;
        error_scope = s.errors;

        // Input gets its own connection so observing never drains Unity's X event queue.
        input_display = XOpenDisplay(s.display_name.c_str());
        if (!locking(input_display) ||
            !separate_input_connection(reinterpret_cast<uintptr_t>(display), reinterpret_cast<uintptr_t>(input_display))) return false;
        if (!own_window(display, original)) return false;

        int event_base = 0;
        int error_base = 0;
        int shape_major = 0;
        int shape_minor = 0;
        if (!XShapeQueryExtension(display, &event_base, &error_base) || !XShapeQueryVersion(display, &shape_major, &shape_minor) ||
            shape_major < 1 || (shape_major == 1 && shape_minor < 1)) return false;

        XWindowAttributes a{};
        if (!XGetWindowAttributes(display, original, &a) || a.map_state != IsViewable || a.width != int(w) || a.height != int(h)) return false;

        int count = 0;
        int query[] = {GLX_FBCONFIG_ID, s.config, None};

        GLXFBConfig* configs = glXChooseFBConfig(display, s.screen, query, &count);
        if (!configs || count != 1) {
            if (configs) XFree(configs);
            return false;
        }

        XVisualInfo* visual = glXGetVisualFromFBConfig(display, configs[0]);
        if (!visual || visual->depth != 24) {
            if (visual) XFree(visual);
            XFree(configs);
            return false;
        }

        colormap = XCreateColormap(display, a.root, visual->visual, AllocNone);
        XSetWindowAttributes attrs{};
        attrs.colormap = colormap;
        attrs.background_pixmap = None;
        attrs.override_redirect = True;

        child = XCreateWindow(display, original, 0, 0, w, h, 0, visual->depth, InputOutput, visual->visual,
                              CWColormap | CWBackPixmap | CWOverrideRedirect, &attrs);
        XShapeCombineRectangles(display, child, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);

        // -1 survives a failed query, so only a real empty input shape passes below.
        int n = -1;
        int ordering = 0;
        XRectangle* input = XShapeGetRectangles(display, child, ShapeInput, &n, &ordering);
        if (input) XFree(input);

        auto create_context = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
        int attributes[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, s.major, GLX_CONTEXT_MINOR_VERSION_ARB, s.minor,
                            GLX_CONTEXT_PROFILE_MASK_ARB, s.profile, None};

        if (create_context && n == 0) context = create_context(display, configs[0], s.context, True, attributes);
        if (context) drawable = glXCreateWindow(display, configs[0], child, nullptr);
        XFree(visual);
        XFree(configs);
        XSync(display, False);

        if (!good(*this) || !context || !drawable || !glXIsDirect(display, context) ||
            !glXMakeContextCurrent(display, drawable, drawable, context)) return false;

        const GLubyte* worker_renderer = glGetString(GL_RENDERER);
        const GLubyte* worker_vendor = glGetString(GL_VENDOR);
        const GLubyte* worker_version = glGetString(GL_VERSION);
        if (!worker_renderer || !worker_vendor || !worker_version) return false;

        renderer = reinterpret_cast<const char*>(worker_renderer);
        vendor = reinterpret_cast<const char*>(worker_vendor);
        version = reinterpret_cast<const char*>(worker_version);
        if (s.renderer != renderer) return false;

        GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
        if (!vs || !fs) return false;

        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) return false;

        glGenVertexArrays(1, &vao);
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        XSync(display, False);
        return good(*this);
    }

    geometry_outcome worker_glx::geometry(geometry_ticket expected, uint64_t* facts) {
        XWindowAttributes a{};
        int result = XGetWindowAttributes(display, original, &a);
        facts[1] = result;
        facts[2] = a.map_state;
        facts[3] = a.width;
        facts[4] = a.height;

        if (error_scope) {
            x_error_snapshot e = error_scope->read();
            facts[5] = e.count;
            facts[6] = e.last;
        } else {
            facts[5] = 1; // no error trap means the window cannot be trusted
        }

        return classify_geometry(result != 0, facts[5], a.map_state, a.width, a.height, expected);
    }

    bool worker_glx::healthy() {
        return good(*this);
    }

    int worker_glx::scene_available() {
        if (scene_fence) {
            const GLenum status = glClientWaitSync(scene_fence, 0, 0);
            if (status == GL_WAIT_FAILED) return -1;
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return 0;
            glDeleteSync(scene_fence);
            scene_fence = nullptr;
        }
        const int64_t now = native_now();
        if (now >= scene_rate_check) {
            scene_rate_check = now + 1000000000;
            scene_interval = 16666667;
            const char* extensions = glXQueryExtensionsString(display, screen);
            if (extensions && std::strstr(extensions, "GLX_OML_sync_control")) {
                auto rate = reinterpret_cast<PFNGLXGETMSCRATEOMLPROC>(glXGetProcAddressARB(
                    reinterpret_cast<const GLubyte*>("glXGetMscRateOML")));
                int32_t numerator = 0, denominator = 0;
                if (rate && rate(display, drawable, &numerator, &denominator) && numerator > 0 && denominator > 0) {
                    const double hz = double(numerator) / denominator;
                    if (hz >= 20 && hz <= 1000) scene_interval = int64_t(1000000000. / hz);
                }
            }
        }
        if (now < scene_next) return 0;
        scene_started = now;
        return 1;
    }

    bool worker_glx::scene_submitted() {
        if (scene_fence) return false;
        scene_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        scene_next = scene_started + scene_interval;
        return scene_fence != nullptr && glGetError() == GL_NO_ERROR;
    }

    draw_outcome worker_glx::draw(geometry_ticket ticket, const layer& base, const layer& world, const layer& hud, const layer& cache,
                                  const affine& desired, uint32_t logical_width, uint32_t logical_height,
                                  const selection::geometry& selection_geometry,
                                  const smf_scene::frame* scene_frame, const smf_scene::view* scene_view) {
        for (auto& fact : draw_facts) fact = 0;
        draw_facts[0] = 1;

        geometry_outcome outcome = geometry(ticket, draw_facts);
        if (outcome != geometry_outcome::ready) {
            return outcome == geometry_outcome::stale ? draw_outcome::stale_geometry : draw_outcome::failed;
        }

        width = ticket.width;
        height = ticket.height;
        draw_facts[0] = 2;

        affine screen_to_world;
        if (!inverse(desired, screen_to_world)) return draw_outcome::failed;
        draw_facts[0] = 3;

        layer composed{};
        if (scene_frame) {
            if (!scene_view) return draw_outcome::failed;
            const int available = scene_available();
            if (available <= 0) return available < 0 ? draw_outcome::failed : draw_outcome::deferred;
            if (!scene_ready) {
                if (!scene.initialize()) return draw_outcome::failed;
                scene_ready = true;
            }
            if (!scene.draw(*scene_frame, *scene_view)) return draw_outcome::failed;
            composed.texture = scene.texture();
            composed.width = scene_view->width;
            composed.height = scene_view->height;
        } else if (scene_ready) {
            if (scene_fence) {
                const GLenum result = glClientWaitSync(scene_fence, 0, 0);
                if (result == GL_WAIT_FAILED) return draw_outcome::failed;
                if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) return draw_outcome::deferred;
                glDeleteSync(scene_fence);
                scene_fence = nullptr;
            }
            // Ground frames no longer need scene-sized scratch, but keep compiled programs warm.
            scene.clear_textures();
            scene_next = 0;
        }

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glViewport(0, 0, width, height);

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        if (!logical_width) logical_width = width;
        if (!logical_height) logical_height = height;
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, sampler);
        glUniform1i(glGetUniformLocation(program, "raster"), 0);
        glUniform2f(glGetUniformLocation(program, "output_size"), logical_width, logical_height);
        glUniform2f(glGetUniformLocation(program, "pixel_scale"), float(logical_width) / width, float(logical_height) / height);

        auto draw_layer = [&](const layer& l, const affine& map, bool opaque) {
            if (!l.texture) return;
            glBindTexture(GL_TEXTURE_2D, l.texture);
            glUniform2f(glGetUniformLocation(program, "raster_size"), l.width, l.height);
            glUniform3f(glGetUniformLocation(program, "map_x"), map.a, map.b, map.c);
            glUniform3f(glGetUniformLocation(program, "map_y"), map.d, map.e, map.f);
            glUniform1i(glGetUniformLocation(program, "row_flip"), l.flip ? 1 : 0);
            glUniform1i(glGetUniformLocation(program, "opaque"), opaque ? 1 : 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        };

        glUniform1i(glGetUniformLocation(program, "solid"), 0);
        glDisable(GL_BLEND);

        if (!scene_frame && cache.texture) {
            draw_layer(cache, affine{0, 0, .5, 0, 0, .5}, true);
            draw_layer(cache, multiply(cache.source, screen_to_world), true);
        }

        if (scene_frame) draw_layer(composed, affine{}, true);
        else draw_layer(base, world.texture ? multiply(base.source, screen_to_world) : affine{}, true);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        draw_layer(world, multiply(world.source, screen_to_world), false);

        if (selection_geometry.visible) {
            const auto& c = selection_geometry.color;
            glUniform1i(glGetUniformLocation(program, "solid"), 1);
            glUniform4f(glGetUniformLocation(program, "solid_color"), c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]);
            glEnable(GL_SCISSOR_TEST);

            for (const auto& e : selection_geometry.edges) {
                int left = int(std::max(0.f, std::min(float(width), e.left)));
                int right = int(std::max(0.f, std::min(float(width), e.right)));
                int top = int(std::max(0.f, std::min(float(height), e.top)));
                int bottom = int(std::max(0.f, std::min(float(height), e.bottom)));
                if (right > left && bottom > top) {
                    glScissor(left, int(height) - bottom, right - left, bottom - top);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }
            }

            glDisable(GL_SCISSOR_TEST);
            glUniform1i(glGetUniformLocation(program, "solid"), 0);
        }

        draw_layer(hud, affine{}, false);
        glBindTexture(GL_TEXTURE_2D, 0);

        // The caller owns the swap; this only records how the draw went.
        bool okay = good(*this);
        draw_facts[0] = okay ? 4 : 3;
        if (error_scope) {
            x_error_snapshot e = error_scope->read();
            draw_facts[5] = e.count;
            draw_facts[6] = e.last;
        }

        return okay ? draw_outcome::drawn : draw_outcome::failed;
    }

    hidden_surface worker_glx::take_initial_hidden() {
        hidden_surface out{child, drawable, colormap, width, height};
        child = 0;
        colormap = 0;
        return out;
    }

    geometry_outcome worker_glx::create_hidden(uint32_t w, uint32_t h, hidden_surface& out) {
        if (out.window || out.drawable || out.colormap || !display || !w || !h || w > 16384 || h > 16384) return geometry_outcome::failed;

        uint64_t facts[8]{};
        geometry_outcome outcome = geometry({w, h}, facts);
        if (outcome != geometry_outcome::ready) return outcome; // nothing is allocated on stale geometry

        int count = 0;
        int attributes[] = {GLX_FBCONFIG_ID, config, None};

        GLXFBConfig* configs = glXChooseFBConfig(display, screen, attributes, &count);
        if (!configs || count != 1) {
            if (configs) XFree(configs);
            return geometry_outcome::failed;
        }

        XVisualInfo* visual = glXGetVisualFromFBConfig(display, configs[0]);
        if (!visual || visual->depth != 24) {
            if (visual) XFree(visual);
            XFree(configs);
            return geometry_outcome::failed;
        }

        out.width = w;
        out.height = h;
        out.colormap = XCreateColormap(display, RootWindow(display, screen), visual->visual, AllocNone);

        XSetWindowAttributes values{};
        values.colormap = out.colormap;
        values.background_pixmap = None;
        values.override_redirect = True;

        out.window = XCreateWindow(display, original, 0, 0, w, h, 0, visual->depth, InputOutput, visual->visual,
                                   CWColormap | CWBackPixmap | CWOverrideRedirect, &values);
        XShapeCombineRectangles(display, out.window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
        out.drawable = glXCreateWindow(display, configs[0], out.window, nullptr);

        XFree(visual);
        XFree(configs);
        XSync(display, False);
        return out.window && out.drawable && good(*this) ? geometry_outcome::ready : geometry_outcome::failed;
    }

    bool worker_glx::destroy_hidden(hidden_surface& surface) {
        if (!surface.window && !surface.drawable && !surface.colormap) return true;
        if (!surface.source_departed || surface.source_fence || (surface.window && glXGetCurrentDrawable() == surface.window) ||
            (surface.drawable && glXGetCurrentDrawable() == surface.drawable)) return false;

        if (!surface.delete_submitted) {
            if (surface.drawable) glXDestroyWindow(display, surface.drawable);
            if (surface.window) XDestroyWindow(display, surface.window);
            if (surface.colormap) XFreeColormap(display, surface.colormap);
            surface.delete_submitted = true;
        }

        XSync(display, False);
        if (!good(*this)) return false;
        surface = {};
        return true;
    }

    int worker_glx::retire_objects() {
        if (!display || glXGetCurrentDisplay() != display || glXGetCurrentContext() != context) return -1;
        if (objects_retired) return 1;

        if (!object_deletes_issued) {
            object_deletes_issued = true;
            glBindSampler(0, 0);
            glBindVertexArray(0);
            glUseProgram(0);
            if (sampler) glDeleteSamplers(1, &sampler);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (program) glDeleteProgram(program);
            scene.release();
            scene_ready = false;
            if (scene_fence) glDeleteSync(scene_fence);
            scene_fence = nullptr;

            // Cleared so a retry can never delete a name the driver has reused.
            sampler = 0;
            vao = 0;
            program = 0;

            object_retirement_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
        }

        if (!object_retirement_fence) return -1;
        GLenum result = glClientWaitSync(object_retirement_fence, 0, 0);
        if (result == GL_WAIT_FAILED) return -1;
        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) return 0;

        glDeleteSync(object_retirement_fence);
        object_retirement_fence = nullptr;
        objects_retired = true;
        return 1;
    }

    void worker_glx::close_input() {
        if (may_close_input_connection(reinterpret_cast<uintptr_t>(display), reinterpret_cast<uintptr_t>(input_display))) {
            XCloseDisplay(input_display);
        }
        input_display = nullptr;
    }

    bool worker_glx::destroy() {
        uint64_t before = error_scope ? error_scope->read().count : 0;

        if (display) {
            if (context && glXGetCurrentContext() == context) {
                if (sampler) glDeleteSamplers(1, &sampler);
                if (vao) glDeleteVertexArrays(1, &vao);
                if (program) glDeleteProgram(program);

                // A context that stays current is never destroyed; keep everything for a retry.
                if (!glXMakeContextCurrent(display, None, None, nullptr) || glXGetCurrentContext() == context) {
                    close_input();
                    return false;
                }
            }

            if (drawable) glXDestroyWindow(display, drawable);
            if (context) glXDestroyContext(display, context);
            if (child) XDestroyWindow(display, child);
            if (colormap) XFreeColormap(display, colormap);
            XSync(display, False);
        }

        close_input();
        if (error_scope && error_scope->read().count != before) return false; // an X error leaves ownership unclear
        error_scope.reset(); // the last owner unlinks the handler under the display lock

        // The display is borrowed from Unity and is never closed here.
        display = nullptr;
        context = nullptr;
        child = 0;
        drawable = 0;
        colormap = 0;
        return true;
    }

}
