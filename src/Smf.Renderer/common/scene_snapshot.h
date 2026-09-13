#pragma once
#include "scene_packets.h"
#include <array>
#include <cmath>
#include <cstring>

namespace smf_scene {

    struct snapshot {
        description frame{};
        std::array<image, maximum_images> images{};
        std::array<layer, maximum_layers> layers{};
        std::array<effect, maximum_effects> effects{};
    };

    inline bool valid_image(const image& value, uint64_t frame) {
        return value.texture && value.serial && value.serial <= frame &&
            value.width && value.height && value.width <= 16384 && value.height <= 16384 &&
            uint64_t(value.width) * value.height <= 8 * 1024 * 1024 &&
            !(value.flags & ~(flip_y | linear_filter | depth_image | reversed_depth)) &&
            (!(value.flags & reversed_depth) || (value.flags & depth_image)) && !value.reserved;
    }

    inline bool image_index(const snapshot& value, uint32_t index) {
        return index < value.frame.image_count;
    }

    inline bool same_size(const image& a, const image& b) {
        return a.width == b.width && a.height == b.height;
    }

    inline bool valid_layer(const snapshot& value, const layer& item) {
        if (!image_index(value, item.color) || !image_index(value, item.probe) || (item.flags & ~depth_occlusion) ||
            item.color == item.probe) return false;

        const auto& color = value.images[item.color];
        const auto& probe = value.images[item.probe];
        if (!same_size(color, probe) || color.serial != probe.serial ||
            ((color.flags | probe.flags) & depth_image)) return false;

        for (double v : item.affine) {
            if (!std::isfinite(v)) return false;
        }

        const double determinant = item.affine[0] * item.affine[4] - item.affine[1] * item.affine[3];
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-10 ||
            !std::isfinite(item.source_x) || !std::isfinite(item.source_z) ||
            !std::isfinite(item.parallax_x) || !std::isfinite(item.parallax_z)) return false;

        const bool depth_layer = item.kind == parallax_map || item.kind == live_parallax_map;
        if (depth_layer) {
            if (item.flags != depth_occlusion || !image_index(value, item.depth) ||
                !std::isfinite(item.depth_x) || !std::isfinite(item.depth_z) || !std::isfinite(item.depth_scale) ||
                !std::isfinite(item.depth_offset) || std::abs(item.depth_scale) < 1e-8) return false;
            const auto& depth = value.images[item.depth];
            if (!same_size(color, depth) || color.serial != depth.serial || !(depth.flags & depth_image)) return false;
        } else if (item.flags || item.depth != no_image || item.depth_x || item.depth_z || item.depth_scale || item.depth_offset) {
            return false;
        }

        if (item.kind == live_map || item.kind == live_parallax_map) {
            if (!image_index(value, item.reference)) return false;
            const auto& reference = value.images[item.reference];
            return same_size(color, reference) && color.serial == value.frame.source_frame &&
                reference.serial == color.serial && !(reference.flags & depth_image) &&
                item.parallax_x == 0 && item.parallax_z == 0;
        }

        if (item.kind == cached_map)
            return !item.flags && item.reference == no_image && item.parallax_x == 0 && item.parallax_z == 0;
        return item.kind == parallax_map && item.reference == no_image;
    }

    inline bool valid_projection(const float* value) {
        for (unsigned i = 0; i < 16; ++i) {
            if (!std::isfinite(value[i])) return false;
        }

        return std::abs(value[0]) > 1e-7 && std::abs(value[5]) > 1e-7 &&
            std::abs(value[11]) > 1e-7 && std::abs(value[15]) < 1e-7;
    }

    inline bool valid_background(const snapshot& value, uint32_t width, uint32_t height) {
        const auto& b = value.frame.world;
        if (!image_index(value, b.live_color) || !image_index(value, b.sky) || b.view_count != 4 ||
            (b.flags & ~negative_clip_depth) || b.reserved || b.reserved0 || b.reserved1 ||
            b.live_glow_count > maximum_background_glows || b.cached_glow_count > maximum_background_glows ||
            !valid_projection(b.projection)) return false;

        const auto& live = value.images[b.live_color];
        const auto& sky = value.images[b.sky];
        if (live.width != width || live.height != height || live.serial != value.frame.source_frame ||
            ((live.flags | sky.flags) & depth_image)) return false;

        const double fields[] = {b.source_x, b.source_z, b.min_x, b.max_x,
            b.min_z, b.max_z, b.camera_x_per_cell, b.camera_y_per_cell, b.sky_scale};
        for (double field : fields) {
            if (!std::isfinite(field)) return false;
        }

        if (!(b.min_x < b.max_x && b.min_z < b.max_z && b.sky_scale >= 1 && b.sky_scale <= 4)) return false;
        if (b.sky_scale == 1 && (!same_size(live, sky) || sky.serial != value.frame.source_frame)) return false;

        for (uint32_t i = 0; i < b.view_count; ++i) {
            const auto& view = b.views[i];
            if (!image_index(value, view.color) || !image_index(value, view.depth) || view.flags || view.reserved ||
                !valid_projection(view.projection) || !std::isfinite(view.x) || !std::isfinite(view.z) ||
                view.x != ((i & 1) ? b.max_x : b.min_x) || view.z != ((i & 2) ? b.max_z : b.min_z)) return false;
            const auto& color = value.images[view.color];
            const auto& depth = value.images[view.depth];
            if (!same_size(color, depth) || color.serial != depth.serial ||
                !(depth.flags & depth_image) || (color.flags & depth_image)) return false;
            if (i) {
                const auto& first = value.images[b.views[0].color];
                if (!same_size(first, color) || first.serial != color.serial) return false;
            }
        }

        for (uint32_t group = 0; group < 2; ++group) {
            const auto* glows = group ? b.cached_glows : b.live_glows;
            const uint32_t count = group ? b.cached_glow_count : b.live_glow_count;
            for (uint32_t i = 0; i < count; ++i) {
                const auto& glow = glows[i];
                for (float field : glow.world_origin) if (!std::isfinite(field)) return false;
                for (float field : glow.planet_origin) if (!std::isfinite(field)) return false;
                for (float field : glow.sun) if (!std::isfinite(field)) return false;
                for (float field : glow.mesh_center) if (!std::isfinite(field)) return false;
                if (!std::isfinite(glow.planet_radius) || glow.planet_radius <= 0 ||
                    !std::isfinite(glow.glow_radius) || glow.glow_radius <= 0 ||
                    !std::isfinite(glow.intensity) || !std::isfinite(glow.mesh_radius) || glow.mesh_radius <= 0) return false;
            }
        }

        return true;
    }

    inline bool valid_effect(const snapshot& value, const effect& item) {
        if (item.flags || item.reserved) return false;
        for (float v : item.parameters) {
            if (!std::isfinite(v)) return false;
        }

        if (item.kind == color_correction) {
            if (!image_index(value, item.first_image) || item.second_image != no_image || item.program || item.pass)
                return false;
            const auto& input = value.images[item.first_image];
            if (input.flags & depth_image) return false;
            return input.width == 256 && input.height == 4;
        }

        if (item.kind == additive_image) {
            if (!image_index(value, item.first_image) || !image_index(value, item.second_image) ||
                item.first_image == item.second_image || item.program || item.pass) return false;
            return !((value.images[item.first_image].flags | value.images[item.second_image].flags) & depth_image);
        }

        if (item.kind != image_filter || !item.program) return false;
        return (item.first_image == no_image || image_index(value, item.first_image)) &&
            (item.second_image == no_image || image_index(value, item.second_image));
    }

    inline bool read_snapshot(const description* input, uint64_t source_frame,
        uint32_t width, uint32_t height, snapshot& output) {
        if (!input || input->size != sizeof(description) || input->version != version ||
            !source_frame || input->source_frame != source_frame || input->flags || input->reserved ||
            !input->image_count || input->image_count > maximum_images ||
            input->layer_count < 2 || input->layer_count > maximum_layers || input->effect_count > maximum_effects ||
            !input->images || !input->layers || (input->effect_count && !input->effects) ||
            (input->images & 7) || (input->layers & 7) || (input->effects & 7)) return false;

        snapshot candidate{};
        candidate.frame = *input;
        std::memcpy(candidate.images.data(), reinterpret_cast<const void*>(input->images), input->image_count * sizeof(image));
        std::memcpy(candidate.layers.data(), reinterpret_cast<const void*>(input->layers), input->layer_count * sizeof(layer));
        if (input->effect_count)
            std::memcpy(candidate.effects.data(), reinterpret_cast<const void*>(input->effects), input->effect_count * sizeof(effect));

        for (uint32_t i = 0; i < input->image_count; ++i) {
            if (!valid_image(candidate.images[i], source_frame)) return false;
        }

        uint32_t live = 0, cached = 0, parallax = 0, live_parallax = 0;
        const layer* first_parallax = nullptr;
        for (uint32_t i = 0; i < input->layer_count; ++i) {
            const auto& item = candidate.layers[i];
            if (!valid_layer(candidate, item)) return false;
            if (item.kind == live_map || item.kind == live_parallax_map) {
                if (item.kind == live_map) ++live;
                else ++live_parallax;
                const auto& color = candidate.images[item.color];
                if (color.width != width || color.height != height) return false;
            }
            if (item.kind == cached_map) ++cached;
            if (item.kind == parallax_map) {
                ++parallax;
                if (first_parallax) {
                    const auto& first_depth = candidate.images[first_parallax->depth];
                    const auto& next_depth = candidate.images[item.depth];
                    if (item.depth_x != first_parallax->depth_x || item.depth_z != first_parallax->depth_z ||
                        item.depth_scale != first_parallax->depth_scale || item.depth_offset != first_parallax->depth_offset ||
                        next_depth.serial != first_depth.serial ||
                        ((next_depth.flags ^ first_depth.flags) & reversed_depth)) return false;
                } else {
                    first_parallax = &item;
                }
            }
        }

        if (live != 1 || cached != 1 || live_parallax != (parallax ? 1u : 0u) ||
            !valid_background(candidate, width, height)) return false;
        for (uint32_t i = 0; i < input->effect_count; ++i) {
            if (!valid_effect(candidate, candidate.effects[i])) return false;
        }

        // The ticket owns the arrays now; no borrowed address survives admission.
        candidate.frame.images = candidate.frame.layers = candidate.frame.effects = 0;
        output = candidate;
        return true;
    }

}
