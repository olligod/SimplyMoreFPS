#include "scene_compositor.h"
#include "scene_shaders.h"
#include "../common/projection_math.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace smf_scene {

    namespace {

        struct affine { double a, b, c, d, e, f; };

        struct background_constants {
            float projection[16]{};
            float inverse_projection[16]{};
            float delta[4]{};
            float size[4]{};
        };

        struct parallax_constants {
            float x[4]{}, y[4]{}, size[4]{};
            uint32_t flags[4]{};
            float inverse_x[4]{}, inverse_z[4]{}, live_x[4]{}, live_y[4]{}, depth[4]{};
        };

        struct constants {
            float rows[4][4]{};
            float dimensions[2][4]{};
            uint32_t image_flags[4][4]{};
            float values[4]{};
            float inverse_projection[16]{};
            float projection[16]{};
            float camera_delta[4]{};
            background_constants backgrounds[4]{};
            uint32_t glow_counts[4]{};
            background_glow live_glows[maximum_background_glows]{};
            background_glow cached_glows[maximum_background_glows]{};
            parallax_constants parallax{};
            float live_inverse_x[4]{}, live_inverse_z[4]{}, live_depth[4]{};
            float output_size[4]{};
        };

        bool invert_matrix(const float* matrix, float* output) {
            double rows[4][8]{};
            for (size_t row = 0; row < 4; ++row) {
                for (size_t column = 0; column < 4; ++column) {
                    if (!std::isfinite(matrix[column * 4 + row])) return false;
                    rows[row][column] = matrix[column * 4 + row];
                }
                rows[row][row + 4] = 1;
            }
            for (size_t column = 0; column < 4; ++column) {
                size_t pivot = column;
                for (size_t row = column + 1; row < 4; ++row) {
                    if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
                }
                if (std::abs(rows[pivot][column]) < 1e-12) return false;
                for (size_t i = 0; i < 8; ++i) std::swap(rows[column][i], rows[pivot][i]);
                const double divisor = rows[column][column];
                for (size_t i = 0; i < 8; ++i) rows[column][i] /= divisor;
                for (size_t row = 0; row < 4; ++row) {
                    if (row == column) continue;
                    const double scale = rows[row][column];
                    for (size_t i = 0; i < 8; ++i) rows[row][i] -= rows[column][i] * scale;
                }
            }
            for (size_t row = 0; row < 4; ++row) {
                for (size_t column = 0; column < 4; ++column) {
                    output[column * 4 + row] = static_cast<float>(rows[row][column + 4]);
                    if (!std::isfinite(output[column * 4 + row])) return false;
                }
            }
            return true;
        }

        bool sampling_rows(const layer& source, const view& desired, const image& size, float* x, float* y) {
            const auto& m = desired.map_affine;
            for (double value : source.affine) if (!std::isfinite(value)) return false;
            for (double value : desired.map_affine) if (!std::isfinite(value)) return false;
            const double determinant = m[0] * m[4] - m[1] * m[3];
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return false;
            affine transform = smf_projection::inverse(affine{m[0], m[1], m[2], m[3], m[4], m[5]}, determinant);
            if (source.kind == parallax_map) {
                transform.c -= (desired.camera_x - source.source_x) * source.parallax_x;
                transform.f -= (desired.camera_z - source.source_z) * source.parallax_z;
            }
            const auto& a = source.affine;
            transform = smf_projection::multiply(affine{a[0], a[1], a[2], a[3], a[4], a[5]}, transform);
            x[0] = static_cast<float>(transform.a / size.width);
            x[1] = static_cast<float>(transform.b / size.width);
            x[2] = static_cast<float>(transform.c / size.width);
            y[0] = static_cast<float>(transform.d / size.height);
            y[1] = static_cast<float>(transform.e / size.height);
            y[2] = static_cast<float>(transform.f / size.height);
            for (size_t i = 0; i < 3; ++i) if (!std::isfinite(x[i]) || !std::isfinite(y[i])) return false;
            return true;
        }

        bool inverse_rows(const layer& source, float* x, float* z) {
            const auto& a = source.affine;
            const double determinant = a[0] * a[4] - a[1] * a[3];
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return false;
            const affine inverse = smf_projection::inverse(affine{a[0], a[1], a[2], a[3], a[4], a[5]}, determinant);
            x[0] = static_cast<float>(inverse.a);
            x[1] = static_cast<float>(inverse.b);
            x[2] = static_cast<float>(inverse.c);
            z[0] = static_cast<float>(inverse.d);
            z[1] = static_cast<float>(inverse.e);
            z[2] = static_cast<float>(inverse.f);
            for (size_t i = 0; i < 3; ++i) if (!std::isfinite(x[i]) || !std::isfinite(z[i])) return false;
            return true;
        }

        bool depth_parameters(const layer& source, float* output) {
            output[0] = static_cast<float>(source.depth_x);
            output[1] = static_cast<float>(source.depth_z);
            output[2] = static_cast<float>(source.depth_scale);
            output[3] = static_cast<float>(source.depth_offset);
            for (size_t i = 0; i < 4; ++i) if (!std::isfinite(output[i])) return false;
            return std::abs(output[2]) > 1e-8;
        }

        GLuint compile(GLenum stage, const char* text) {
            GLuint shader = glCreateShader(stage);
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint okay = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
            if (!okay) {
                char message[4096]{};
                glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
                std::fprintf(stderr, "SMF scene shader: %s\n", message);
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        GLuint make_program(const char* entry) {
            const std::string text = std::string("#version 420 core\n#define ") + entry + "\n" + scene_fragment;
            GLuint vertex = compile(GL_VERTEX_SHADER, scene_vertex);
            GLuint fragment = compile(GL_FRAGMENT_SHADER, text.c_str());
            if (!vertex || !fragment) {
                if (vertex) glDeleteShader(vertex);
                if (fragment) glDeleteShader(fragment);
                return 0;
            }
            GLuint program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint okay = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &okay);
            if (!okay) {
                char message[4096]{};
                glGetProgramInfoLog(program, sizeof(message), nullptr, message);
                std::fprintf(stderr, "SMF scene program: %s\n", message);
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

    }

    struct scene_compositor::state {
        struct target {
            GLuint texture = 0;
            uint32_t width = 0, height = 0;
            void release() {
                if (texture) glDeleteTextures(1, &texture);
                texture = 0;
                width = height = 0;
            }
            bool create(uint32_t w, uint32_t h, GLenum format) {
                if (texture && width == w && height == h) return true;
                release();
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, format, w, h);
                width = w;
                height = h;
                return texture && glGetError() == GL_NO_ERROR;
            }
        };

        struct depth_cache {
            std::array<target, 4> levels;
            uint32_t count = 0, width = 0, height = 0, flags = 0;
            uint64_t texture = 0;
            uint64_t serial = 0;
        };

        GLuint programs[9]{};
        GLuint buffer = 0, vao = 0, fbo = 0, samplers[2]{};
        std::array<target, 2> targets;
        std::array<std::array<target, 3>, 2> nearest;
        std::array<target, 2> candidates;
        std::array<target, 2> detail;
        std::array<depth_cache, 4> depths;
        uint32_t width = 0, height = 0, current = 0;
        bool output_valid = false;

        enum stage { background, map_layer, nearest_layer, candidate, ambiguity, resolve, correction, additive, reduce };

        ~state() {
            for (auto& item : targets) item.release();
            for (auto& set : nearest) for (auto& item : set) item.release();
            for (auto& item : candidates) item.release();
            for (auto& item : detail) item.release();
            for (auto& cache : depths) for (auto& item : cache.levels) item.release();
            for (auto program : programs) if (program) glDeleteProgram(program);
            if (buffer) glDeleteBuffers(1, &buffer);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (fbo) glDeleteFramebuffers(1, &fbo);
            glDeleteSamplers(2, samplers);
        }

        bool create_targets(uint32_t w, uint32_t h, bool parallax, bool live) {
            for (auto& item : targets) if (!item.create(w, h, GL_RGBA16F)) return false;
            if (parallax) {
                for (auto& set : nearest) for (auto& item : set) if (!item.create(w, h, GL_RGBA32F)) return false;
            } else {
                for (auto& set : nearest) for (auto& item : set) item.release();
            }
            if (live) {
                for (auto& item : candidates) if (!item.create(w, h, GL_RGBA32F)) return false;
                for (auto& item : detail) if (!item.create(w, h, GL_RGBA32F)) return false;
            } else {
                for (auto& item : candidates) item.release();
                for (auto& item : detail) item.release();
            }
            width = w;
            height = h;
            return true;
        }

        void bind_outputs(const GLuint* outputs, uint32_t count, uint32_t w, uint32_t h) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            GLenum attachments[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
            for (uint32_t i = 0; i < 3; ++i)
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachments[i], GL_TEXTURE_2D, i < count ? outputs[i] : 0, 0);
            glDrawBuffers(count, attachments);
            glViewport(0, 0, w, h);
        }

        bool run(stage operation, constants data, const std::array<GLuint, 15>& images,
            const GLuint* outputs, uint32_t count, uint32_t w, uint32_t h) {
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            bind_outputs(outputs, count, w, h);
            if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            data.output_size[0] = static_cast<float>(w);
            data.output_size[1] = static_cast<float>(h);
            glBindBuffer(GL_UNIFORM_BUFFER, buffer);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(data), &data);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
            glUseProgram(programs[operation]);
            glBindVertexArray(vao);
            for (uint32_t i = 0; i < images.size(); ++i) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, images[i]);
                glBindSampler(i, samplers[(data.image_flags[i / 4][i % 4] & linear_filter) != 0]);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
            for (uint32_t i = 0; i < images.size(); ++i) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindSampler(i, 0);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glActiveTexture(GL_TEXTURE0);
            return glGetError() == GL_NO_ERROR;
        }

        bool run(stage operation, constants data, const std::array<GLuint, 15>& images, bool first = false) {
            const uint32_t next = first ? 0 : 1 - current;
            if (!run(operation, data, images, &targets[next].texture, 1, width, height)) return false;
            current = next;
            return true;
        }

        bool update_depth(uint32_t index, const image& metadata, GLuint input, uint32_t flags) {
            auto& cache = depths[index];
            if (cache.texture == metadata.texture && cache.serial == metadata.serial && cache.width == metadata.width &&
                cache.height == metadata.height && cache.flags == flags)
                return true;
            cache.serial = 0;
            uint32_t w = metadata.width, h = metadata.height;
            constants data{};
            data.values[2] = 1;
            data.values[3] = (flags & negative_clip_depth) != 0;
            cache.count = 0;
            do {
                data.dimensions[0][0] = static_cast<float>(w);
                data.dimensions[0][1] = static_cast<float>(h);
                w = (w + 15) / 16;
                h = (h + 15) / 16;
                if (cache.count == cache.levels.size()) return false;
                auto& item = cache.levels[cache.count++];
                if (!item.create(w, h, GL_R32F)) return false;
                std::array<GLuint, 15> inputs{};
                inputs[3] = input;
                if (!run(reduce, data, inputs, &item.texture, 1, w, h)) return false;
                input = item.texture;
                data.values[2] = 0;
            } while (w > 1 || h > 1);
            for (size_t i = cache.count; i < cache.levels.size(); ++i) cache.levels[i].release();
            cache.width = metadata.width;
            cache.height = metadata.height;
            cache.flags = flags;
            cache.texture = metadata.texture;
            cache.serial = metadata.serial;
            return true;
        }
    };

    scene_compositor::scene_compositor() = default;
    scene_compositor::~scene_compositor() = default;
    scene_compositor::scene_compositor(scene_compositor&&) noexcept = default;
    scene_compositor& scene_compositor::operator=(scene_compositor&&) noexcept = default;

    bool scene_compositor::initialize() {
        if (state_) return false;
        GLint units = 0, targets = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &targets);
        if (units < 15 || targets < 3) return false;
        std::unique_ptr<state> next(new (std::nothrow) state);
        if (!next) return false;
        const char* entries[] = {"BACKGROUND", "MAP_LAYER", "NEAREST", "CANDIDATE", "AMBIGUITY", "PARALLAX_RESOLVE", "CORRECTION", "ADDITIVE", "REDUCE"};
        for (size_t i = 0; i < std::size(entries); ++i) {
            next->programs[i] = make_program(entries[i]);
            if (!next->programs[i]) return false;
        }
        glGenBuffers(1, &next->buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, next->buffer);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(constants), nullptr, GL_DYNAMIC_DRAW);
        glGenVertexArrays(1, &next->vao);
        glGenFramebuffers(1, &next->fbo);
        glGenSamplers(2, next->samplers);
        for (uint32_t i = 0; i < 2; ++i) {
            glSamplerParameteri(next->samplers[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(next->samplers[i], GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(next->samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(next->samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (glGetError() != GL_NO_ERROR) return false;
        state_ = std::move(next);
        return true;
    }

    bool scene_compositor::draw(const frame& input, const view& desired) {
        if (!state_ || !input.scene || !input.resources || !desired.width || !desired.height ||
            desired.width > 16384 || desired.height > 16384 ||
            !std::isfinite(desired.camera_x) || !std::isfinite(desired.camera_z)) return false;
        auto& s = *state_;
        s.output_valid = false;
        const auto& source = *input.scene;
        const auto& scene = source.frame;
        if (!scene.image_count || scene.image_count > maximum_images || scene.layer_count > maximum_layers ||
            scene.effect_count > maximum_effects) return false;
        const layer* live = nullptr;
        const layer* cache = nullptr;
        const layer* live_parallax = nullptr;
        std::array<const layer*, maximum_layers> parallax{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < scene.layer_count; ++i) {
            const auto& item = source.layers[i];
            if (item.kind == live_map) live = &item;
            if (item.kind == cached_map) cache = &item;
            if (item.kind == live_parallax_map) live_parallax = &item;
            if (item.kind == parallax_map) parallax[count++] = &item;
        }
        if (!live || !cache || count > maximum_layers) return false;
        for (uint32_t i = 0; i < scene.effect_count; ++i) if (source.effects[i].kind == image_filter) return false;
        if (!s.create_targets(desired.width, desired.height, count != 0, live_parallax != nullptr)) return false;

        const auto bind = [&](constants& data, std::array<GLuint, 15>& images, uint32_t slot, uint32_t index) {
            images[slot] = input.resources[index];
            data.image_flags[slot / 4][slot % 4] = source.images[index].flags;
        };
        const auto& world = scene.world;
        if (world.live_glow_count > maximum_background_glows || world.cached_glow_count > maximum_background_glows)
            return false;
        constants data{};
        data.glow_counts[0] = world.live_glow_count;
        data.glow_counts[1] = world.cached_glow_count;
        std::memcpy(data.live_glows, world.live_glows, sizeof(data.live_glows));
        std::memcpy(data.cached_glows, world.cached_glows, sizeof(data.cached_glows));
        if (!invert_matrix(world.projection, data.inverse_projection)) return false;
        std::memcpy(data.projection, world.projection, sizeof(data.projection));
        data.values[0] = static_cast<float>(world.view_count);
        data.values[1] = static_cast<float>(world.sky_scale);
        data.values[3] = (world.flags & negative_clip_depth) != 0;
        const double camera_x = std::clamp(desired.camera_x, world.min_x, world.max_x);
        const double camera_z = std::clamp(desired.camera_z, world.min_z, world.max_z);
        data.camera_delta[0] = static_cast<float>((camera_x - std::clamp(world.source_x, world.min_x, world.max_x)) * world.camera_x_per_cell);
        data.camera_delta[1] = static_cast<float>((camera_z - std::clamp(world.source_z, world.min_z, world.max_z)) * world.camera_y_per_cell);
        data.values[2] = data.camera_delta[0] == 0 && data.camera_delta[1] == 0;
        std::array<GLuint, 15> images{};
        bind(data, images, 1, world.live_color);
        bind(data, images, 2, world.sky);
        std::array<uint32_t, 4> order{0, 1, 2, 3};
        const auto distance = [&](uint32_t i) {
            const double x = (camera_x - world.views[i].x) * world.camera_x_per_cell;
            const double z = (camera_z - world.views[i].z) * world.camera_y_per_cell;
            return x * x + z * z;
        };
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return distance(a) != distance(b) ? distance(a) < distance(b) : a < b; });
        for (uint32_t i = 0; i < 4; ++i) {
            const auto& captured = world.views[order[i]];
            auto& fields = data.backgrounds[i];
            if (!invert_matrix(captured.projection, fields.inverse_projection)) return false;
            std::memcpy(fields.projection, captured.projection, sizeof(fields.projection));
            fields.delta[0] = static_cast<float>((camera_x - captured.x) * world.camera_x_per_cell);
            fields.delta[1] = static_cast<float>((camera_z - captured.z) * world.camera_y_per_cell);
            const auto& depth = source.images[captured.depth];
            fields.size[0] = static_cast<float>(depth.width);
            fields.size[1] = static_cast<float>(depth.height);
            bind(data, images, 3 + i * 2, captured.color);
            bind(data, images, 4 + i * 2, captured.depth);
            if (!s.update_depth(order[i], depth, input.resources[captured.depth], world.flags)) return false;
            const auto& bounds = s.depths[order[i]];
            images[11 + i] = bounds.levels[bounds.count - 1].texture;
        }
        if (!s.run(state::background, data, images, true)) return false;

        std::array<constants, maximum_layers> fields{};
        uint32_t nearest = 0;
        for (uint32_t i = 0; i < count; ++i) {
            auto& row = fields[i];
            const auto& item = *parallax[i];
            const auto& color = source.images[item.color];
            auto& values = row.parallax;
            row.values[0] = static_cast<float>(i);
            row.values[1] = i == 0;
            if (!sampling_rows(item, desired, color, values.x, values.y) ||
                !inverse_rows(item, values.inverse_x, values.inverse_z) || !depth_parameters(item, values.depth)) return false;
            values.size[0] = static_cast<float>(color.width);
            values.size[1] = static_cast<float>(color.height);
            values.flags[0] = color.flags;
            values.flags[1] = source.images[item.probe].flags;
            values.flags[2] = source.images[item.depth].flags;
            std::array<GLuint, 15> inputs{};
            bind(row, inputs, 1, item.color);
            bind(row, inputs, 2, item.probe);
            bind(row, inputs, 3, item.depth);
            inputs[4] = s.nearest[nearest][0].texture;
            inputs[5] = s.nearest[nearest][1].texture;
            inputs[6] = s.nearest[nearest][2].texture;
            nearest = 1 - nearest;
            GLuint outputs[] = {s.nearest[nearest][0].texture, s.nearest[nearest][1].texture, s.nearest[nearest][2].texture};
            if (!s.run(state::nearest_layer, row, inputs, outputs, 3, s.width, s.height)) return false;
        }

        uint32_t detail = 0;
        if (live_parallax) {
            GLuint outputs[] = {s.candidates[0].texture, s.candidates[1].texture, s.detail[0].texture};
            s.bind_outputs(outputs, 3, s.width, s.height);
            const float clear[4]{};
            for (int i = 0; i < 3; ++i) glClearBufferfv(GL_COLOR, i, clear);
            for (uint32_t i = 0; i < count; ++i) {
                constants row = fields[i];
                const auto& color = source.images[live_parallax->color];
                row.dimensions[0][0] = static_cast<float>(color.width);
                row.dimensions[0][1] = static_cast<float>(color.height);
                if (!inverse_rows(*live_parallax, row.live_inverse_x, row.live_inverse_z) || !depth_parameters(*live_parallax, row.live_depth)) return false;
                layer live_projection = *live_parallax;
                live_projection.kind = parallax_map;
                live_projection.parallax_x = parallax[i]->parallax_x;
                live_projection.parallax_z = parallax[i]->parallax_z;
                if (!sampling_rows(live_projection, desired, color, row.parallax.live_x, row.parallax.live_y)) return false;
                std::array<GLuint, 15> inputs{};
                bind(row, inputs, 1, live_parallax->color);
                bind(row, inputs, 2, live_parallax->probe);
                bind(row, inputs, 3, live_parallax->reference);
                bind(row, inputs, 4, live_parallax->depth);
                inputs[5] = s.nearest[nearest][0].texture;
                if (!s.run(state::candidate, row, inputs, outputs, 3, s.width, s.height)) return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                constants row = fields[i];
                std::array<GLuint, 15> inputs{};
                inputs[0] = s.detail[detail].texture;
                bind(row, inputs, 3, parallax[i]->depth);
                detail = 1 - detail;
                if (!s.run(state::ambiguity, row, inputs, &s.detail[detail].texture, 1, s.width, s.height)) return false;
            }
        }
        if (count) {
            constants row{};
            row.values[0] = live_parallax != nullptr;
            std::array<GLuint, 15> inputs{};
            inputs[0] = s.targets[s.current].texture;
            inputs[1] = s.nearest[nearest][1].texture;
            inputs[2] = s.nearest[nearest][2].texture;
            inputs[3] = s.candidates[0].texture;
            inputs[4] = s.candidates[1].texture;
            inputs[5] = s.detail[detail].texture;
            if (!s.run(state::resolve, row, inputs)) return false;
        }

        constants row{};
        std::array<GLuint, 15> inputs{};
        inputs[0] = s.targets[s.current].texture;
        bind(row, inputs, 1, live->color);
        bind(row, inputs, 2, live->probe);
        bind(row, inputs, 3, live->reference);
        bind(row, inputs, 4, cache->color);
        bind(row, inputs, 5, cache->probe);
        row.values[0] = 1;
        row.values[1] = live->source_x == desired.camera_x && live->source_z == desired.camera_z &&
            source.images[live->color].width == desired.width && source.images[live->color].height == desired.height &&
            std::equal(live->affine, live->affine + 6, desired.map_affine);
        row.dimensions[0][0] = static_cast<float>(source.images[live->color].width);
        row.dimensions[0][1] = static_cast<float>(source.images[live->color].height);
        row.dimensions[1][0] = static_cast<float>(source.images[cache->color].width);
        row.dimensions[1][1] = static_cast<float>(source.images[cache->color].height);
        if (!sampling_rows(*live, desired, source.images[live->color], row.rows[0], row.rows[1]) ||
            !sampling_rows(*cache, desired, source.images[cache->color], row.rows[2], row.rows[3]) ||
            !s.run(state::map_layer, row, inputs)) return false;

        for (uint32_t i = 0; i < scene.effect_count; ++i) {
            const auto& effect = source.effects[i];
            constants effect_data{};
            std::array<GLuint, 15> effect_images{};
            effect_images[0] = s.targets[s.current].texture;
            bind(effect_data, effect_images, 1, effect.first_image);
            effect_data.values[0] = effect.parameters[0];
            state::stage operation = state::correction;
            if (effect.kind == additive_image) {
                operation = state::additive;
                bind(effect_data, effect_images, 4, effect.second_image);
                if (!sampling_rows(*live, desired, source.images[effect.first_image], effect_data.rows[0], effect_data.rows[1]) ||
                    !sampling_rows(*cache, desired, source.images[effect.second_image], effect_data.rows[2], effect_data.rows[3])) return false;
            }
            if (!s.run(operation, effect_data, effect_images)) return false;
        }
        s.output_valid = true;
        return true;
    }

    GLuint scene_compositor::texture() const {
        return state_ && state_->output_valid ? state_->targets[state_->current].texture : 0;
    }

    void scene_compositor::clear_textures() {
        if (!state_ || !state_->width) return;
        auto& s = *state_;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
        for (uint32_t i = 0; i < 3; ++i)
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        for (auto& item : s.targets) item.release();
        for (auto& set : s.nearest) for (auto& item : set) item.release();
        for (auto& item : s.candidates) item.release();
        for (auto& item : s.detail) item.release();
        for (auto& cache : s.depths) {
            for (auto& item : cache.levels) item.release();
            cache = {};
        }
        s.width = s.height = s.current = 0;
        s.output_valid = false;
    }

    void scene_compositor::release() { state_.reset(); }

}
