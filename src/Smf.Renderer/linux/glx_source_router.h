#pragma once
#include "linux_glx.h"
#include "linux_unity_swap_slot.h"
#include "session_handoff.h"
#include <atomic>

namespace linux_session {

    // Hooks Unity's GLX table so the source context can be routed to a hidden
    // window. Lives for the process; the public methods run only inside Unity's
    // end-of-frame callback, and no worker ever makes Unity's context current.
    class glx_source_router {
        using make_fn = Bool (*)(Display*, GLXDrawable, GLXContext);
        using swap_fn = void (*)(Display*, GLXDrawable);
        using query_fn = void (*)(Display*, GLXDrawable, int, unsigned*);
        using interval_fn = void (*)(Display*, GLXDrawable, int);

        source_glx* source = nullptr;
        unity_swap_slot engine{};
        Display* display = nullptr;
        GLXContext context = nullptr;
        GLXDrawable original = 0;
        uint32_t source_thread = 0;
        uint32_t hook_mask = 0;
        std::shared_ptr<x_error_ledger> errors;
        make_fn make = nullptr;
        swap_fn swap = nullptr;
        query_fn query = nullptr;
        interval_fn interval = nullptr;
        std::atomic<Window> hidden_window{0};
        std::atomic<bool> routing{false};
        std::atomic<bool> terminal{false};
        frame_key armed{};
        frame_key submitted{};
        GLsync fence = nullptr;
        bool server_processed = false;
        uint64_t restore_floor = 0;
        uint64_t restore_serial = 0;
        int fault_code = 0;
        uint32_t wait_result = 0;

        static glx_source_router* live;
        static Bool make_hook(Display*, GLXDrawable, GLXContext);
        static void swap_hook(Display*, GLXDrawable);
        static void query_hook(Display*, GLXDrawable, int, unsigned*);
        static void interval_hook(Display*, GLXDrawable, int);
        bool own() const;

    public:
        int install(source_glx& source); // before a worker is started

        void set_prepared_hidden(Window value) {
            hidden_window.store(value, std::memory_order_release);
        }

        bool rebind_hidden(); // the worker has already unbound the hidden surface
        bool rebind_original(uint64_t frame, uint64_t serial); // the worker has already unbound the original
        bool replace_hidden(Window target, GLsync& old_source_work); // the worker keeps the old surface until the fence completes
        bool arm_original(const frame_key& complete_native_frame);
        bool poll_original(presented& completed);
        bool restore_hooks(); // source owner, nothing armed or pending

        void quit() { // never dereferences the engine table again
            terminal.store(true, std::memory_order_release);
        }

        bool routing_hidden() const {
            return routing.load(std::memory_order_acquire);
        }

        bool marker_pending() const {
            return armed.frame || fence;
        }

        bool installed() const {
            return hook_mask != 0;
        }

        int fault() const {
            return fault_code;
        }

        uint32_t last_wait() const {
            return wait_result;
        }
    };

    // Presents the worker's activation frame on the original window and waits for
    // it from the worker context. The XSync happens once per activation, not per frame.
    class worker_presentation {
        presented value{};
        GLsync fence = nullptr;
        int fault_code = 0;
        uint32_t wait_result = 0;

    public:
        bool swap_and_arm(worker_glx&, const frame_key&); // performs the swap itself
        bool poll(worker_glx&, presented&);

        bool pending() const {
            return fence != nullptr;
        }

        int fault() const {
            return fault_code;
        }

        uint32_t last_wait() const {
            return wait_result;
        }
    };

}
