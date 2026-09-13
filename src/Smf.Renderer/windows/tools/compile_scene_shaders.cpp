#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include "../scene_shaders.h"

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fputs("Expected an output header path.\n", stderr);
        return 1;
    }
    std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fputs("Could not open the output header.\n", stderr);
        return 1;
    }
    output << "#pragma once\n#include <cstddef>\n\nnamespace smf_scene_shaders {\n\n"
        "    struct program {\n        const unsigned char* data;\n        std::size_t size;\n    };\n\n";

    const char* entries[] = {"vertex", "backgroundPixel", "layerPixel", "parallaxPixel",
        "correctionPixel", "additivePixel", "reduceDepth"};
    const D3D_SHADER_MACRO reduce[] = {{"REDUCE_DEPTH", "1"}, {nullptr, nullptr}};
    for (const char* entry : entries) {
        const bool vertex = std::strcmp(entry, "vertex") == 0;
        const bool depth = std::strcmp(entry, "reduceDepth") == 0;
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        const HRESULT result = D3DCompile(smf_scene_source::source, sizeof(smf_scene_source::source) - 1,
            nullptr, depth ? reduce : nullptr, nullptr, entry, vertex ? "vs_5_0" : depth ? "cs_5_0" : "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            0, &code, &errors);
        if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        if (FAILED(result)) {
            std::fprintf(stderr, "Could not compile %s: %08lx\n", entry, static_cast<unsigned long>(result));
            return 1;
        }

        output << "    inline constexpr unsigned char " << entry << "_bytes[] = {";
        const auto* bytes = static_cast<const unsigned char*>(code->GetBufferPointer());
        for (std::size_t i = 0; i < code->GetBufferSize(); ++i) {
            if (i % 16 == 0) output << "\n        ";
            output << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]) << ',';
        }
        output << "\n    };\n    inline constexpr program " << entry << '{' << entry
            << "_bytes, sizeof(" << entry << "_bytes)};\n\n";
        std::printf("Compiled %s.\n", entry);
        std::fflush(stdout);
    }

    output << "}\n";
    output.close();
    if (!output) {
        std::fputs("Could not write the output header.\n", stderr);
        return 1;
    }
    return 0;
}
