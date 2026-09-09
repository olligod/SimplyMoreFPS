#pragma once
#include <cstddef>
#include <cstdint>

namespace linux_session {

    struct original_window {
        uint32_t size = 256;
        uint32_t version = 1;
        int32_t result = 1;
        uint32_t stage = 0;
        uint32_t pid = 0;
        uint32_t thread = 0;
        uint32_t scanned = 0;
        uint32_t candidates = 0;
        uint64_t window = 0;
        uint64_t root = 0;
        uint64_t parent = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t map_state = 0;
        uint32_t x_errors = 0;
        uint32_t last_x_error = 0;
        uint32_t limit = 0;
        uint32_t flags = 0;
        uint64_t reserved[5]{};
        char wm_class[128]{};
    };

    static_assert(sizeof(original_window) == 256 && offsetof(original_window, window) == 32 &&
                  offsetof(original_window, wm_class) == 128, "OriginalWindow256");

    struct window_candidate {
        uint64_t window = 0;
        uint64_t root = 0;
        uint64_t parent = 0;
        uint32_t pid = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        bool input_output = false;
        bool viewable = false;
        bool owned_ancestor = false;
        bool ewmh_listed = false;
    };

    inline bool eligible_window(const window_candidate& c, uint32_t pid) {
        return c.window && c.root && c.pid == pid && pid && c.input_output && c.viewable && !c.owned_ancestor &&
               c.width && c.height && c.width <= 16384 && c.height <= 16384;
    }

    struct window_selection {
        window_candidate selected{};
        uint32_t count = 0;

        void observe(const window_candidate& c, uint32_t pid) {
            if (!eligible_window(c, pid) || c.window == selected.window) return;
            if (!count) selected = c;
            ++count;
        }

        int result(bool complete, bool error_free) const {
            if (!error_free) return -4;
            if (!complete) return -5;
            if (count > 1) return 2;
            return count == 1 ? 0 : 1;
        }
    };

}

// Main thread only. Opens its own X connection and never touches Unity or GL.
extern "C" int smf_linux_find_original_window(linux_session::original_window*, uint32_t);
extern "C" int smf_session_find_original_window(uint64_t*);
