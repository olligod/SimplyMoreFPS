#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

// Visual state of the drag-select rectangle. Unity still owns the real drag and selection.
#pragma pack(push, 8)
struct smf_selection_state {
    uint32_t size;
    uint32_t version;
    uint64_t session;
    uint64_t content;
    uint64_t drag;
    uint64_t publication;
    int32_t map_id;
    uint32_t active;
    double x;
    double z;
    float ui_scale;
    float color[4];
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(smf_selection_state) == 88, "selection_state 88");

namespace selection {

    struct edge {
        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;
    };

    struct geometry {
        edge edges[4]{};
        float color[4]{};
        bool visible = false;
    };

    // Main thread publishes, worker reads. Neither side ever blocks on the other.
    class mailbox {
    public:
        // 0 stored, 1 busy (try again next publication), -1 rejected.
        int publish(const smf_selection_state* value, uint32_t bytes) {
            if (!value || bytes != sizeof(*value) || value->size != sizeof(*value) || value->version != 1) return -1;
            if (!value->publication || value->active > 1 || value->reserved) return -1;
            if (value->active && (!value->session || !value->content || !value->drag || value->map_id < 0)) return -1;
            if (!std::isfinite(value->x) || !std::isfinite(value->z)) return -1;
            if (!std::isfinite(value->ui_scale) || value->ui_scale < .25f || value->ui_scale > 8) return -1;
            for (float c : value->color) {
                if (!std::isfinite(c) || c < 0 || c > 1) return -1;
            }

            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock.owns_lock()) return 1;
            if (value->publication <= latest.publication) return -1;
            latest = *value;
            return 0;
        }

        void read(smf_selection_state& value) {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (lock.owns_lock()) value = latest;
        }

    private:
        std::mutex gate;
        smf_selection_state latest{};
    };

    // Leaked on purpose so static destruction never races the worker.
    inline mailbox& state() {
        static auto* value = new mailbox();
        return *value;
    }

    class worker {
    public:
        // Latch before sampling OS input, so a drag published after the sample cannot be
        // dismissed by the older button state.
        smf_selection_state latch() {
            state().read(cached);
            return cached;
        }

        template <class Affine>
        geometry read(uint64_t session, uint64_t content, int32_t map_id, const Affine& projection,
            bool focused, bool pointer_valid, bool left_held, double pointer_x, double pointer_y) {
            return read(latch(), session, content, map_id, projection, focused, pointer_valid, left_held, pointer_x, pointer_y);
        }

        template <class Affine>
        geometry read(const smf_selection_state& main, uint64_t session, uint64_t content, int32_t map_id,
            const Affine& projection, bool focused, bool pointer_valid, bool left_held,
            double pointer_x, double pointer_y) {
            geometry result;
            if (main.publication != cached.publication || !main.active) return result;
            if (!focused || !pointer_valid || !left_held) dismissed_drag = std::max(dismissed_drag, main.drag);
            if (main.drag <= dismissed_drag || main.session != session || main.content != content || main.map_id != map_id) {
                return result;
            }

            const double start_x = projection.a * main.x + projection.b * main.z + projection.c;
            const double start_y = projection.d * main.x + projection.e * main.z + projection.f;
            const double determinant = projection.a * projection.e - projection.b * projection.d;
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-10) return result;

            // Invert the projection to measure the drag in world cells; under half a cell is not a drag.
            const double px = pointer_x - start_x;
            const double py = pointer_y - start_y;
            const double dx = (projection.e * px - projection.b * py) / determinant;
            const double dz = (projection.a * py - projection.d * px) / determinant;
            if (!std::isfinite(dx) || !std::isfinite(dz) || dx * dx + dz * dz <= .25) return result;

            const double left = std::min(start_x, pointer_x);
            const double top = std::min(start_y, pointer_y);
            const double right = std::max(start_x, pointer_x);
            const double bottom = std::max(start_y, pointer_y);
            const double thickness = 2 * main.ui_scale;
            auto make_edge = [](double l, double t, double r, double b) {
                return edge{float(std::floor(l)), float(std::floor(t)), float(std::ceil(r)), float(std::ceil(b))};
            };

            result.edges[0] = make_edge(left, top, right, top + thickness);
            result.edges[1] = make_edge(left, bottom - thickness, right, bottom);
            result.edges[2] = make_edge(left, top, left + thickness, bottom);
            result.edges[3] = make_edge(right - thickness, top, right, bottom);

            std::copy(main.color, main.color + 4, result.color);
            result.visible = true;
            return result;
        }

    private:
        smf_selection_state cached{};
        uint64_t dismissed_drag = 0;
    };

}
