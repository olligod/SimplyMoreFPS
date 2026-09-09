#pragma once
#include <cstdint>

namespace linux_session {

    inline bool can_pump_stopped(bool source_retired, bool worker_live, bool any_lease, bool pending_dispatch, bool hooks_installed) {
        return source_retired && !worker_live && !any_lease && !pending_dispatch && !hooks_installed;
    }

    // Who currently draws to the original window. Ordered: comparisons below rely on it.
    enum class handoff_phase {
        native,
        worker_staging,
        hidden_released,
        source_hidden,
        worker_original,
        worker_released,
        source_restored,
        native_fresh,
        retired
    };

    struct frame_key {
        uint64_t session = 0;
        uint64_t generation = 0;
        uint64_t content = 0;
        uint64_t frame = 0;
        uint64_t restore = 0;

        bool operator==(const frame_key& other) const {
            return session == other.session && generation == other.generation && content == other.content &&
                   frame == other.frame && restore == other.restore;
        }
    };

    // Facts about one presented frame, filled in by the context that swapped it.
    struct presented {
        frame_key key{};
        uint64_t original = 0;
        bool exclusive = false;
        bool swap_returned = false;
        bool server_processed = false;
        bool gpu_complete = false;
        bool x_error = false;

        bool complete(uint64_t expected_window) const {
            return original == expected_window && exclusive && swap_returned && server_processed && gpu_complete && !x_error;
        }
    };

    class surface_handoff {
        handoff_phase current = handoff_phase::native;
        uint64_t window = 0;
        uint64_t session = 0;
        uint64_t restore_serial = 0;
        uint64_t restore_floor = 0;

    public:
        handoff_phase phase() const {
            return current;
        }

        void reset(uint64_t original, uint64_t identity) {
            window = original;
            session = identity;
            current = handoff_phase::native;
            restore_serial = 0;
            restore_floor = 0;
        }

        bool worker_staged() {
            if (current != handoff_phase::native) return false;
            current = handoff_phase::worker_staging;
            return true;
        }

        bool worker_released_hidden(bool gpu_complete, bool x_processed, bool unbound) {
            if (current != handoff_phase::worker_staging || !gpu_complete || !x_processed || !unbound) return false;
            current = handoff_phase::hidden_released;
            return true;
        }

        bool source_bound_hidden(bool exact_render_owner, bool real_rebind) {
            if (current != handoff_phase::hidden_released || restore_serial || !exact_render_owner || !real_rebind) return false;
            current = handoff_phase::source_hidden;
            return true;
        }

        bool worker_bound_original(bool real_bind) {
            if (current != handoff_phase::source_hidden || restore_serial || !real_bind) return false;
            current = handoff_phase::worker_original;
            return true;
        }

        bool warmup_ready(const presented& shown, const frame_key& prepared) const {
            return (current == handoff_phase::native || current == handoff_phase::worker_staging) && shown.key == prepared &&
                   shown.key.session == session && shown.key.restore == 0 && shown.complete(window);
        }

        bool activation_ready(const presented& shown, const frame_key& prepared) const {
            return current == handoff_phase::worker_original && restore_serial == 0 && shown.key == prepared &&
                   shown.key.session == session && shown.complete(window);
        }

        bool begin_restore(uint64_t serial) {
            if (!serial || serial <= restore_serial || current == handoff_phase::retired) return false;
            restore_serial = serial;
            return true;
        }

        bool worker_released_original(bool no_draw_in_flight, bool gpu_complete, bool x_processed, bool unbound) {
            if (!restore_serial || current == handoff_phase::retired || !no_draw_in_flight || !gpu_complete || !x_processed || !unbound) return false;
            current = handoff_phase::worker_released;
            return true;
        }

        bool source_bound_original(uint64_t frame, bool exact_render_owner, bool real_rebind) {
            if (current != handoff_phase::worker_released || !frame || !exact_render_owner || !real_rebind) return false;
            restore_floor = frame;
            current = handoff_phase::source_restored;
            return true;
        }

        // Acknowledges restore routing only; a fresh native frame needs native_available.
        bool routing_acknowledged(uint64_t serial) const {
            return serial == restore_serial && restore_floor && current >= handoff_phase::source_restored;
        }

        bool native_available(const presented& shown, uint64_t requested_floor, uint64_t requested_content) {
            // Content only has to be at or past the request; session, restore serial and window stay exact.
            if (current != handoff_phase::source_restored || shown.key.session != session || shown.key.generation != 0 ||
                !shown.key.content || shown.key.content < requested_content || shown.key.restore != restore_serial ||
                shown.key.frame <= restore_floor || shown.key.frame <= requested_floor || !shown.complete(window)) return false;
            current = handoff_phase::native_fresh;
            return true;
        }

        bool retired(bool all_leases_gone, bool source_hooks_restored, bool worker_joined) {
            if (current != handoff_phase::native_fresh || !all_leases_gone || !source_hooks_restored || !worker_joined) return false;
            current = handoff_phase::retired;
            return true;
        }

        bool retired_without_worker(bool no_worker_ever_started, bool all_leases_gone, bool no_hooks) {
            if (!restore_serial || !no_worker_ever_started || !all_leases_gone || !no_hooks) return false;
            if (current != handoff_phase::native && current != handoff_phase::source_restored) return false;
            current = handoff_phase::retired;
            return true;
        }
    };

}
