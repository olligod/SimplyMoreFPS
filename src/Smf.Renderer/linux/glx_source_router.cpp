#include "glx_source_router.h"
#include "clock.h"
#include "swap_trace.h"
#include <X11/Xlibint.h>
#undef min
#undef max
#include <cstring>
#include <dlfcn.h>

namespace linux_session {
    namespace {

        // Kept for the process lifetime, like the hook owner that writes into it.
        swap_trace& trace = *new swap_trace();

        bool finished(GLsync fence, int& fault, uint32_t& wait_result) {
            if (!fence) return false;

            GLenum result = glClientWaitSync(fence, 0, 0);
            wait_result = result;
            if (result == GL_WAIT_FAILED) fault = -203;
            return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
        }

    }

    glx_source_router* glx_source_router::live = nullptr;

    void reset_swap_trace(uint64_t session) {
        trace.reset(session);
    }

    bool glx_source_router::own() const {
        return source && native_thread() == source_thread && glXGetCurrentDisplay() == display && glXGetCurrentContext() == context;
    }

    Bool glx_source_router::make_hook(Display* d, GLXDrawable x, GLXContext c) {
        auto& self = *live;
        bool routed = d == self.display && x == self.original && c == self.context && self.routing.load();
        return self.make(d, routed ? self.hidden_window.load() : x, c);
    }

    void glx_source_router::query_hook(Display* d, GLXDrawable x, int attribute, unsigned* result) {
        auto& self = *live;
        bool routed = d == self.display && x == self.original && self.routing.load();
        self.query(d, routed ? self.hidden_window.load() : x, attribute, result);
    }

    void glx_source_router::interval_hook(Display* d, GLXDrawable x, int value) {
        auto& self = *live;
        bool routed = d == self.display && x == self.original && self.routing.load();
        self.interval(d, routed ? self.hidden_window.load() : x, value);
    }

    void glx_source_router::swap_hook(Display* d, GLXDrawable x) {
        auto& self = *live;
        bool ours = d == self.display && x == self.original && self.own();
        bool diverting = self.routing.load();
        bool hidden = ours && diverting;
        GLXDrawable chosen = hidden ? self.hidden_window.load() : x;
        Display* current_display = glXGetCurrentDisplay();
        GLXContext current_context = glXGetCurrentContext();
        uint32_t thread = native_thread();
        bool same_context = current_display == self.display && current_context == self.context;
        swap_route route = classify_swap_route(d == self.display && x == self.original, diverting, ours, same_context);

        swap_sample sample;
        auto& w = sample.words;
        w[0] = trace.enter(route);
        w[1] = native_now();
        w[3] = thread;
        w[4] = self.source_thread;
        w[5] = reinterpret_cast<uintptr_t>(d);
        w[6] = x;
        w[7] = reinterpret_cast<uintptr_t>(current_display);
        w[8] = reinterpret_cast<uintptr_t>(current_context);
        w[9] = glXGetCurrentDrawable();
        w[10] = glXGetCurrentReadDrawable();
        w[11] = reinterpret_cast<uintptr_t>(self.context);
        w[12] = self.original;
        w[13] = chosen;
        w[14] = self.hidden_window.load();
        w[15] = (diverting ? 1u : 0u) | (ours ? 2u : 0u) | (self.terminal.load() ? 4u : 0u) | (same_context ? 8u : 0u) |
                (thread != self.source_thread ? 16u : 0u);

        self.swap(d, chosen);
        w[2] = native_now();
        trace.finish(route, sample);

        // Terminal keeps routing but starts no new marker work.
        if (!ours || self.terminal.load()) return;
        if (hidden) return;
        if (!self.armed.frame || self.fence || glXGetCurrentDrawable() != self.original) return;

        self.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!self.fence) {
            // Drop the marker rather than let it attach to a later swap.
            self.fault_code = -203;
            self.armed = {};
            return;
        }

        glFlush();
        XSync(d, False);
        self.server_processed = self.errors && self.errors->read().count == 0;
        self.submitted = self.armed;
        self.armed = {};
    }

    int glx_source_router::install(source_glx& unity) {
        if (live && live != this) return -200;
        if (terminal.load() || hook_mask || !unity.own()) return -202;

        source = &unity;
        display = unity.display;
        context = unity.context;
        original = unity.drawable;
        source_thread = unity.thread;
        trace.configure(reinterpret_cast<uintptr_t>(display));
        restore_floor = 0;
        restore_serial = 0;
        fault_code = 0;
        armed = {};
        submitted = {};
        interval = nullptr;
        hidden_window = 0;
        routing = false;

        int found = find_unity_swap_slot(display, original, context, engine);
        if (found) return -1000 + found;

        auto table = reinterpret_cast<void**>(engine.table);
        make = reinterpret_cast<make_fn>(table[0x68 / 8]);
        swap = reinterpret_cast<swap_fn>(table[0x70 / 8]);
        query = reinterpret_cast<query_fn>(table[0x78 / 8]);

        if (table[0x80 / 8]) {
            Dl_info info{};
            if (!dladdr(table[0x80 / 8], &info) || !info.dli_sname || std::strcmp(info.dli_sname, "glXSwapIntervalEXT")) return -201;
            interval = reinterpret_cast<interval_fn>(table[0x80 / 8]);
        }

        const uintptr_t offsets[] = {0x68, 0x70, 0x78, 0x80};
        void* before[] = {reinterpret_cast<void*>(make), reinterpret_cast<void*>(swap), reinterpret_cast<void*>(query),
                          reinterpret_cast<void*>(interval)};
        void* after[] = {reinterpret_cast<void*>(&make_hook), reinterpret_cast<void*>(&swap_hook), reinterpret_cast<void*>(&query_hook),
                         reinterpret_cast<void*>(&interval_hook)};

        live = this;
        for (unsigned i = 0; i < 4; i++) {
            if (!before[i]) continue;

            void* expected = before[i];
            if (!__atomic_compare_exchange_n(reinterpret_cast<void**>(engine.table + offsets[i]), &expected, after[i], false,
                                             __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                // Put back whatever was already hooked before giving up.
                for (unsigned j = 0; j < i; j++) {
                    if (!(hook_mask & (1u << j))) continue;
                    void* installed = after[j];
                    if (__atomic_compare_exchange_n(reinterpret_cast<void**>(engine.table + offsets[j]), &installed, before[j], false,
                                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) hook_mask &= ~(1u << j);
                }

                return -201;
            }

            hook_mask |= 1u << i;
        }

        // One ledger for source, router and worker; separate handlers would eat each other's X errors.
        errors = x_error_ledger::create(display);
        if (!errors) return -202;

        unity.errors = errors;
        return 0;
    }

    bool glx_source_router::rebind_hidden() {
        Window target = hidden_window.load();
        if (!own() || terminal.load() || !target || routing.load() || fence || armed.frame) return false;
        if (glXGetCurrentDrawable() != original || !make(display, target, context)) return false;

        source->drawable = target;
        routing = true;
        return true;
    }

    bool glx_source_router::rebind_original(uint64_t frame, uint64_t serial) {
        if (!own() || terminal.load() || !frame || !serial || serial <= restore_serial || fence || armed.frame) return false;
        if (!make(display, original, context)) return false;

        source->drawable = original;
        routing = false;
        restore_floor = frame;
        restore_serial = serial;
        return true;
    }

    bool glx_source_router::replace_hidden(Window target, GLsync& old_source_work) {
        old_source_work = nullptr;
        if (!own() || terminal.load() || !routing.load() || !target || target == hidden_window.load() || fence || armed.frame) return false;

        old_source_work = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!old_source_work) {
            fault_code = -203;
            return false;
        }

        glFlush();
        XSync(display, False);
        if (!errors || errors->read().count) {
            fault_code = -203;
            return false;
        }

        if (!make(display, target, context)) {
            fault_code = -203;
            return false;
        }

        source->drawable = target;
        hidden_window.store(target, std::memory_order_release);
        return true;
    }

    bool glx_source_router::arm_original(const frame_key& marker) {
        if (!own() || terminal.load() || fault_code || routing.load() || glXGetCurrentDrawable() != original || !marker.session ||
            !marker.frame || fence || armed.frame) return false;
        if (marker.restore && (marker.restore != restore_serial || marker.frame <= restore_floor)) return false;
        if (!marker.restore && restore_serial) return false;

        armed = marker;
        return true;
    }

    bool glx_source_router::poll_original(presented& completed) {
        if (!own() || routing.load() || glXGetCurrentDrawable() != original || !fence || !finished(fence, fault_code, wait_result)) return false;

        completed = {};
        completed.key = submitted;
        completed.original = original;
        completed.exclusive = true;
        completed.swap_returned = true;
        completed.server_processed = server_processed;
        completed.gpu_complete = true;
        completed.x_error = !server_processed;

        glDeleteSync(fence);
        fence = nullptr;
        submitted = {};
        server_processed = false;
        return true;
    }

    bool glx_source_router::restore_hooks() {
        if (!hook_mask) return true;
        if (!own() || routing.load() || fence || armed.frame || glXGetCurrentDrawable() != original) return false;

        // Also re-walks the table chain when a partial install never reached the swap slot.
        if (!restore_unity_swap_slot(engine, display, original, context, reinterpret_cast<void*>(&swap_hook), reinterpret_cast<void*>(swap))) {
            return false;
        }

        hook_mask &= ~2u;

        const uintptr_t offsets[] = {0x68, 0x78, 0x80};
        void* before[] = {reinterpret_cast<void*>(&make_hook), reinterpret_cast<void*>(&query_hook), reinterpret_cast<void*>(&interval_hook)};
        void* after[] = {reinterpret_cast<void*>(make), reinterpret_cast<void*>(query), reinterpret_cast<void*>(interval)};
        const uint32_t bits[] = {1, 4, 8};

        for (unsigned i = 0; i < 3; i++) {
            if (!(hook_mask & bits[i])) continue;

            void* expected = before[i];
            // A slot that already holds the original is fine.
            if (!__atomic_compare_exchange_n(reinterpret_cast<void**>(engine.table + offsets[i]), &expected, after[i], false,
                                             __ATOMIC_RELEASE, __ATOMIC_ACQUIRE) && expected != after[i]) return false;
            hook_mask &= ~bits[i];
        }

        hook_mask = 0;
        errors.reset(); // source and worker keep the ledger alive through their own cleanup
        return true;
    }

    bool worker_presentation::swap_and_arm(worker_glx& worker, const frame_key& key) {
        if (fence || !key.session || !key.frame || worker.drawable != worker.original || glXGetCurrentDisplay() != worker.display ||
            glXGetCurrentContext() != worker.context || glXGetCurrentDrawable() != worker.original) return false;
        if (fault_code) return false;

        glXSwapBuffers(worker.display, worker.original);
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence) {
            fault_code = -203;
            return false;
        }

        glFlush();
        XSync(worker.display, False);

        value = {};
        value.key = key;
        value.original = worker.original;
        value.exclusive = true;
        value.swap_returned = true;
        value.server_processed = worker.healthy();
        value.gpu_complete = false;
        value.x_error = !worker.healthy();
        return true;
    }

    bool worker_presentation::poll(worker_glx& worker, presented& completed) {
        if (glXGetCurrentDisplay() != worker.display || glXGetCurrentContext() != worker.context || !fence ||
            !finished(fence, fault_code, wait_result)) return false;

        glDeleteSync(fence);
        fence = nullptr;
        value.gpu_complete = true;
        completed = value;
        value = {};
        return true;
    }

    extern "C" __attribute__((visibility("default"))) int smf_session_swap_trace(swap_trace_packet* output, uint32_t bytes) {
        if (!output || bytes != sizeof(*output)) return -201;
        return trace.read(*output, uint64_t(native_now())) ? 0 : 1;
    }

}
