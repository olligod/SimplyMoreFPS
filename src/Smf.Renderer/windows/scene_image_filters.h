#pragma once
#include "scene_compositor.h"

namespace smf_scene {

    enum filter_texture_role : uint32_t { main_texture = 1, area_texture, search_texture, blend_texture };
    enum filter_parameter_role : uint32_t {
        main_texel_size = 1, texel_size, subpixel_blending, edge_threshold, edge_threshold_min,
        smaa_threshold, sharpness, object_to_world, matrix_vp
    };

#pragma pack(push, 8)
    struct filter_texture {
        uint32_t role, texture_slot, sampler_slot, reserved;
    };

    struct filter_parameter {
        uint32_t role, offset, rows, columns;
    };

    struct filter_buffer {
        uint32_t slot, size, parameter_count, reserved;
        uint64_t parameters;
    };

    struct filter_stage {
        uint64_t bytecode;
        uint32_t bytecode_size, texture_count;
        uint64_t textures, buffers;
        uint32_t buffer_count, reserved;
    };

    struct filter_pass {
        filter_stage vertex, fragment;
    };

    struct filter_definition {
        uint32_t size, version, pass_count, reserved;
        uint64_t passes;
    };
#pragma pack(pop)

    static_assert(sizeof(filter_texture) == 16 && sizeof(filter_parameter) == 16, "Filter bindings16");
    static_assert(sizeof(filter_buffer) == 24 && sizeof(filter_stage) == 40, "Filter buffer24 stage40");
    static_assert(sizeof(filter_pass) == 80 && sizeof(filter_definition) == 24, "Filter pass80 definition24");

    class scene_image_filters final : public image_filter_executor {
    public:
        scene_image_filters();
        ~scene_image_filters();
        bool supports(uint64_t program, uint32_t pass) const override;
        HRESULT execute(ID3D11DeviceContext* context, const frame& source, const effect& operation,
            ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, uint32_t width, uint32_t height) override;

    private:
        HRESULT execute_frame(ID3D11DeviceContext* context, const frame& source, const effect& operation,
            ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, uint32_t width, uint32_t height);
        struct state;
        std::unique_ptr<state> state_;
    };
}

extern "C" __declspec(dllexport) int __cdecl smf_scene_register_filter(
    const smf_scene::filter_definition* definition, uint64_t* handle);
