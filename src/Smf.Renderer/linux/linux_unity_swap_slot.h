#pragma once
#include <GL/glx.h>
#include <cstdint>

namespace linux_session {

    // Where UnityPlayer.so keeps its GLX function table for the game window.
    struct unity_swap_slot {
        uintptr_t module = 0;
        uintptr_t device = 0;
        uintptr_t table = 0;
        uintptr_t window = 0;
        void** slot = nullptr;
    };

    bool is_glx_entrypoint(void*, const char* name);
    int find_unity_swap_slot(Display*, GLXDrawable, GLXContext, unity_swap_slot&);
    bool restore_unity_swap_slot(const unity_swap_slot&, Display*, GLXDrawable, GLXContext, void* hook, void* original);

}
