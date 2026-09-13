#pragma once
#include <cstddef>
#include <cstdint>

namespace smf_scene {

    constexpr uint32_t version = 1;
    constexpr uint32_t maximum_images = 32;
    constexpr uint32_t maximum_layers = 16;
    constexpr uint32_t maximum_effects = 8;
    constexpr uint32_t maximum_background_glows = 4;
    constexpr uint32_t no_image = UINT32_MAX;

    enum image_flags : uint32_t {
        flip_y = 1,
        linear_filter = 2,
        depth_image = 4,
        reversed_depth = 8
    };

    enum layer_kind : uint32_t {
        live_map = 1,
        cached_map = 2,
        parallax_map = 3,
        live_parallax_map = 4
    };

    enum layer_flags : uint32_t {
        depth_occlusion = 1
    };

    enum effect_kind : uint32_t {
        color_correction = 1,
        additive_image = 2,
        image_filter = 3
    };

    enum background_flags : uint32_t {
        negative_clip_depth = 1
    };

#pragma pack(push, 8)

    struct image {
        uint64_t texture, serial;
        uint32_t width, height, flags, reserved;
    };

    struct layer {
        uint32_t kind, color, probe, reference;
        double affine[6];
        double source_x, source_z, parallax_x, parallax_z;
        uint32_t flags, depth;
        double depth_x, depth_z, depth_scale, depth_offset;
    };

    struct background_view {
        uint32_t color, depth, flags, reserved;
        float projection[16];
        double x, z;
    };

    struct background_glow {
        float world_origin[3], planet_radius;
        float planet_origin[3], glow_radius;
        float sun[3], intensity;
        float mesh_center[3], mesh_radius;
    };

    struct background {
        uint32_t live_color, sky, view_count, flags;
        float projection[16];
        double source_x, source_z;
        double min_x, max_x, min_z, max_z;
        double camera_x_per_cell, camera_y_per_cell;
        double sky_scale, reserved;
        background_view views[4];
        uint32_t live_glow_count, cached_glow_count, reserved0, reserved1;
        background_glow live_glows[maximum_background_glows];
        background_glow cached_glows[maximum_background_glows];
    };

    struct effect {
        uint32_t kind, first_image, second_image, flags;
        uint64_t program;
        uint32_t pass, reserved;
        float parameters[16];
    };

    // Array pointers are borrowed only for admission; the native ticket copies their values.
    struct description {
        uint32_t size, version, image_count, layer_count, effect_count, flags;
        uint64_t source_frame;
        uint64_t images, layers, effects, reserved;
        background world;
    };

#pragma pack(pop)

    static_assert(sizeof(image) == 32, "Scene image32");
    static_assert(sizeof(layer) == 136 && offsetof(layer, affine) == 16 &&
        offsetof(layer, depth_x) == 104, "Scene layer136");
    static_assert(sizeof(background_view) == 96 && offsetof(background_view, x) == 80, "Scene background view96");
    static_assert(sizeof(background_glow) == 64 && offsetof(background_glow, mesh_center) == 48, "Scene glow64");
    static_assert(sizeof(background) == 1072 && offsetof(background, source_x) == 80 &&
        offsetof(background, views) == 160 && offsetof(background, live_glows) == 560, "Scene background1072");
    static_assert(sizeof(effect) == 96 && offsetof(effect, parameters) == 32, "Scene effect96");
    static_assert(sizeof(description) == 1136 && offsetof(description, world) == 64, "Scene description1136");

}
