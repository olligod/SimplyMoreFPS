#pragma once
#include "session_bridge.h"
#include <cmath>
#include <cstring>

// Pure decisions shared by the session code and the CPU tests.
namespace session {

    enum class cancel_decision { invalid, complete, mark };

    inline cancel_decision decide_cancel(int32_t requested, uint64_t next_token, bool empty, int32_t current_token) {
        if (requested <= 0 || static_cast<uint64_t>(requested) >= next_token) return cancel_decision::invalid;
        // Tokens are never reused, so an empty or reused slot means render already consumed it.
        return empty || current_token != requested ? cancel_decision::complete : cancel_decision::mark;
    }

    inline bool visual_bundle_complete(bool background, bool map, bool hud) {
        return background && map && hud;
    }

    inline uint32_t cache_background_source_y(const session_cache& cache) {
        return (cache.flags & 2u) && cache.height ? cache.height - 1 : 0;
    }

    inline bool copy_region_fits(uint32_t source_width, uint32_t source_height, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        return width && height && uint64_t(x) + width <= source_width && uint64_t(y) + height <= source_height;
    }

    inline bool frame_layout_matches(uint32_t bytes, uint32_t size, uint32_t version) {
        return bytes == sizeof(session_frame) && size == bytes && version == 3;
    }

    inline bool cache_valid(const session_frame& frame) {
        const auto& cache = frame.cache;

        if (!(frame.flags & session_has_map)) {
            const session_cache zero{};
            return std::memcmp(&cache, &zero, sizeof(cache)) == 0;
        }

        if (!(cache.flags & 1u) || (cache.flags & ~3u) || cache.reserved || !cache.texture) return false;
        if (!cache.serial || cache.serial > frame.source_frame) return false;
        if (cache.texture == frame.world_texture || cache.texture == frame.hud_texture) return false;
        if (!cache.width || !cache.height || cache.width > 16384 || cache.height > 16384) return false;
        if (uint64_t(cache.width) * cache.height * 4 > 64ull * 1024 * 1024) return false;

        for (double value : cache.affine) {
            if (!std::isfinite(value) || std::abs(value) > 1e7) return false;
        }

        const double det = cache.affine[0] * cache.affine[4] - cache.affine[1] * cache.affine[3];
        return std::isfinite(det) && std::abs(det) > 1e-10;
    }

    enum class cache_update { reject, keep, copy };

    inline cache_update decide_cache_update(const session_cache& current, const session_cache& next) {
        if (!current.serial) return next.serial ? cache_update::copy : cache_update::keep;
        if (next.serial < current.serial || next.width != current.width || next.height != current.height || next.flags != current.flags) {
            return cache_update::reject;
        }
        if (next.serial == current.serial) {
            return std::memcmp(&current, &next, sizeof(current)) == 0 ? cache_update::keep : cache_update::reject;
        }

        return cache_update::copy;
    }

    enum class start_decision { create, already_accepted, busy, stale };

    inline start_decision decide_start(uint64_t previous, uint64_t requested, bool worker, bool sealed, bool same_window) {
        if (!requested || requested < previous) return start_decision::stale;
        if (requested == previous) return worker && !sealed && same_window ? start_decision::already_accepted : start_decision::stale;
        return worker ? start_decision::busy : start_decision::create;
    }

    inline bool accept_content(uint64_t expected_session, uint64_t fence, bool sealed, const session_generation_status& g,
        uint64_t session, uint64_t content, uint64_t generation) {
        return !sealed && session == expected_session && content == fence && g.generation == generation &&
            g.content_revision == content && g.state > 0 && g.state < 4;
    }

    inline bool retain_historical(const session_status& status, uint64_t fence) {
        for (const auto& g : status.generations) {
            if (g.generation == status.active_generation && g.frames && g.content_revision != fence) return true;
        }
        return false;
    }

    inline bool native_handoff_ready(const session_status& status, uint64_t restore_serial, uint64_t after_frame) {
        return restore_serial && (status.flags & 2u) && status.native_present_serial && status.native_backbuffer &&
            status.native_submitted_restore_serial == restore_serial && status.native_submitted_frame > after_frame;
    }

    // Pre-GUI and frame tickets (kinds 1 and 2) pin their generation's textures.
    inline bool ticket_holds_generation(uint32_t kind, uint64_t ticket_generation, uint64_t retired_generation) {
        return (kind == 1 || kind == 2) && ticket_generation == retired_generation;
    }

}
