#pragma once
#include <cstdint>

namespace mac {

    struct source_pass_facts {
        bool present = false;
        uintptr_t texture = 0;
        uint64_t level = 0, slice = 0, depth = 0;
    };

    // An absent render encoder adds no attachment constraint. The caller still
    // validates the renderbuffer texture and keeps the unchanged command buffer
    // for its own ordered blit.
    inline const char* reject_source_pass(const source_pass_facts& pass, uintptr_t source) {
        if (!pass.present) return nullptr;
        if (!pass.texture) return "no-attachment";
        if (!source || pass.texture != source) return "attachment-mismatch";
        if (pass.level) return "attachment-level";
        if (pass.slice) return "attachment-slice";
        if (pass.depth) return "attachment-depth";
        return nullptr;
    }

}
