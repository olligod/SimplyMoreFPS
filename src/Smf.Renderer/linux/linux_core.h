#pragma once
#include "linux_session_packets.h"
#include "../common/projection_math.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

// Policies with no GL, X or Unity dependency; the tests exercise them without a display.
namespace linux_session {

    enum lease_state {
        lease_free,
        lease_source_writing,
        lease_published,
        lease_worker_owned,
        lease_returning,
        lease_quarantined
    };

    struct lease_policy {
        lease_state state = lease_free;

        bool begin() {
            if (state != lease_free) return false;
            state = lease_source_writing;
            return true;
        }

        bool publish(bool real_fence) {
            if (state != lease_source_writing || !real_fence) {
                state = lease_quarantined;
                return false;
            }
            state = lease_published;
            return true;
        }

        bool acquire(bool signaled) {
            if (state != lease_published || !signaled) return false;
            state = lease_worker_owned;
            return true;
        }

        bool hand_back(bool real_fence) {
            if (state != lease_worker_owned || !real_fence) {
                state = lease_quarantined;
                return false;
            }
            state = lease_returning;
            return true;
        }

        bool reclaim(bool signaled) {
            if (state != lease_returning || !signaled) return false;
            state = lease_free;
            return true;
        }

        // A slot still in either source state was never sampled by the worker.
        bool retire_unacquired(bool signaled) {
            if ((state != lease_source_writing && state != lease_published) || !signaled) return false;
            state = lease_free;
            return true;
        }
    };

    // Main marks either completed world GUI calls (flag 2) or their absence (flag 4), never both.
    inline bool valid_world_dispatch(const frame_packet& frame) {
        bool complete = (frame.flags & 2) != 0;
        bool absent = (frame.flags & 4) != 0;
        return complete != absent && ((frame.world_dispatches == 0) == absent);
    }

    // Unity allocates both render textures even on the title screen, so absence
    // is shown by the dispatch flags and a zero pose rather than texture lifetime.
    inline bool valid_absent_world(const frame_packet& frame) {
        pose_packet empty{};
        return !(frame.flags & 1) && (frame.flags & 4) && !(frame.flags & 2) &&
               frame.world_dispatches == 0 && !std::memcmp(&frame.pose, &empty, sizeof(empty));
    }

    // Identity lives in the issued token, not the reusable entry address, so a
    // late cancelled callback cannot consume a replacement packet.
    template<class Packet, size_t N = 32>
    class dispatch_pool {
        struct entry {
            std::atomic<int> token{0};
            Packet packet{};
        };

        std::array<entry, N> entries{};
        std::atomic<int> next{0};

    public:
        int queue(const Packet& value, void** ticket, int* token) {
            *ticket = nullptr;
            *token = 0;
            if (next.load() == std::numeric_limits<int>::max()) return -2;

            for (auto& e : entries) {
                int free = 0;
                if (!e.token.compare_exchange_strong(free, -1)) continue;

                e.packet = value;
                int issued = next.fetch_add(1) + 1;
                *ticket = &e;
                *token = issued;
                e.token.store(issued, std::memory_order_release);
                return 0;
            }

            return 1;
        }

        bool consume(int token, Packet& value) {
            if (token <= 0) return false;

            for (auto& e : entries) {
                int expected = token;
                if (!e.token.compare_exchange_strong(expected, -2, std::memory_order_acquire)) continue;
                value = e.packet;
                e.token.store(0, std::memory_order_release);
                return true;
            }

            return false;
        }

        int cancel(void* ticket, int token) {
            if (token <= 0 || token > next.load()) return -1;

            for (auto& e : entries) {
                if (&e != ticket) continue;
                int expected = token;
                if (e.token.compare_exchange_strong(expected, -2)) e.token.store(0, std::memory_order_release);
                return 0; // cancelling an already consumed or cancelled token is fine
            }

            return -1;
        }

        bool empty() const {
            for (const auto& e : entries) {
                if (e.token.load()) return false;
            }
            return true;
        }

        uint32_t occupied() const {
            uint32_t count = 0;
            for (const auto& e : entries) {
                if (e.token.load()) count++;
            }
            return count;
        }

        // Conservative across generations; never risks packet reuse.
        bool uses(uint64_t generation) const {
            (void)generation;
            return !empty();
        }
    };

    struct affine {
        double a = 1;
        double b = 0;
        double c = 0;
        double d = 0;
        double e = 1;
        double f = 0;
    };

    inline bool valid(const affine& a) {
        const double n[] = {a.a, a.b, a.c, a.d, a.e, a.f};
        for (double v : n) {
            if (!std::isfinite(v) || std::abs(v) > 1e12) return false;
        }
        return std::abs(a.a * a.e - a.b * a.d) > 1e-10;
    }

    inline bool inverse(const affine& a, affine& b) {
        if (!valid(a)) return false;
        double t = a.a * a.e - a.b * a.d;
        b = smf_projection::inverse(a, t);
        return valid(b);
    }

    inline affine multiply(const affine& a, const affine& b) {
        return smf_projection::multiply(a, b);
    }

    inline bool valid_cache(const frame_packet& frame, const cache_packet& previous) {
        const auto& c = frame.cache;

        if (!(frame.flags & 1)) {
            cache_packet empty{};
            return !std::memcmp(&c, &empty, sizeof(c));
        }

        if (!(c.flags & 1) || (c.flags & ~3u) || c.reserved || !c.texture || c.texture > UINT32_MAX || !c.serial ||
            c.serial > frame.source_frame || c.texture == frame.world_texture || c.texture == frame.hud_texture ||
            !c.width || !c.height || c.width > 16384 || c.height > 16384 || uint64_t(c.width) * c.height * 4 > 64ull * 1024 * 1024) return false;
        for (double v : c.affine) {
            if (!std::isfinite(v) || std::abs(v) > 1e7) return false;
        }
        if (!valid({c.affine[0], c.affine[1], c.affine[2], c.affine[3], c.affine[4], c.affine[5]})) return false;
        if (previous.serial && (c.serial < previous.serial || c.width != previous.width || c.height != previous.height ||
                                c.flags != previous.flags)) return false;
        if (c.serial == previous.serial && std::memcmp(&c, &previous, sizeof(c))) return false;

        return true;
    }

    // Screen-pixel affine of a source pose: world X/Z to output pixels, top-left origin.
    inline bool projection(const pose_packet& p, uint32_t width, uint32_t height, affine& out) {
        if (p.size != 264 || p.version != 2 || !p.frame_id || !p.epoch || !p.camera_id || !p.model_revision ||
            p.map_id < 0 || p.reserved || p.root_size <= 0 || !std::isfinite(p.root_size) ||
            !std::isfinite(p.root_x) || !std::isfinite(p.root_y) || !std::isfinite(p.root_z) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.pixel_x || p.pixel_y ||
            p.pixel_width != width || p.pixel_height != height || !std::isfinite(p.orthographic_size) || p.orthographic_size <= 0) return false;

        if (!smf_projection::ground_projection(p.projection, p.world_to_camera, width, height, out)) return false;
        return valid(out);
    }

    // The accepted projection of one generation, from which desired camera poses are derived.
    struct camera_model {
        affine nominal{};
        uint64_t revision = 0;
        uint64_t epoch = 0;
        int map = -1;
        double x = 0;
        double z = 0;
        double logical_root_size = 0;
        double projection_half_height = 0;
        uint32_t width = 0;
        uint32_t height = 0;

        bool root(double xx, double zz, double half_height, affine& out) const {
            if (!revision || !std::isfinite(xx) || !std::isfinite(zz) || !std::isfinite(half_height) || half_height <= 0) return false;

            if (xx == x && zz == z && half_height == projection_half_height) {
                out = nominal;
                return true;
            }

            out = smf_projection::root_projection(nominal, projection_half_height / half_height,
                width, height, x - xx, z - zz);
            return valid(out);
        }

        bool accept(const pose_packet& p, const affine& a, uint32_t w, uint32_t h) {
            if (!revision) {
                nominal = a;
                nominal.c += a.a * (p.x - p.root_x) + a.b * (p.z - p.root_z);
                nominal.f += a.d * (p.x - p.root_x) + a.e * (p.z - p.root_z);

                revision = p.model_revision;
                map = p.map_id;
                x = p.root_x;
                z = p.root_z;
                logical_root_size = p.root_size;
                projection_half_height = p.orthographic_size;
                epoch = p.epoch;
                width = w;
                height = h;
                return true;
            }

            affine expected;
            if (p.epoch != epoch || p.model_revision != revision || p.map_id != map || width != w || height != h ||
                !root(p.root_x, p.root_z, p.orthographic_size, expected)) return false;

            auto near = [](double v, double e) { return std::abs(v - e) <= 1e-6 * (1 + std::abs(e)); };
            return near(a.a, expected.a) && near(a.b, expected.b) && near(a.d, expected.d) && near(a.e, expected.e);
        }
    };

}
