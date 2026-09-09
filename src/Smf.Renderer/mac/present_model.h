#pragma once
#include "session_packets.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// Dependency model for the original layer's own presentation: an ordered EOF
// marker, target work on a drawable, a successful GPU completion and a positive
// presented callback together identify which native frame was displayed.
// Pure data; the caller serializes calls. Nothing here touches Metal or waits.
namespace present_observer {

    struct presented_frame {
        uint64_t session = 0, frame = 0, generation = 0, content = 0, restore = 0, serial = 0;
        uint64_t drawable = 0, acquired_ns = 0, presented_ns = 0;
    };

    enum class fault_kind : uint32_t { none, pool, unknown_route, class_conflict, queue_conflict, identity };

    struct present_counters {
        uint64_t source_epochs = 0, copies = 0, present_calls = 0, presented_positive = 0, presented_zero = 0;
        uint64_t gpu_completed = 0, published = 0, rejected = 0, stale_callbacks = 0, pool_pressure = 0;
        uint64_t unknown_routes = 0, class_conflicts = 0, queue_conflicts = 0, source_writes = 0;
    };

    struct operation {
        enum kind : uint8_t { empty, write, eof, copy };
        kind type = empty;
        uint64_t order = 0, texture = 0, destination = 0, drawable = 0, geometry_epoch = 0;
        session_native_frame marker{};
    };

    struct submission {
        uint64_t id = 0, object = 0, created = 0, enqueue_begin = 0, enqueue_end = 0;
        bool completed = false, good = false, queue_call = false;
        std::array<operation, 16> operations{};
        uint32_t count = 0;
    };

    struct acquisition {
        uint64_t id = 0, object = 0, address = 0, texture = 0, acquired_ns = 0, final_buffer = 0;
        uint64_t source_texture = 0, copy_order = 0, present_buffer = 0, presented_ns = 0;
        uint64_t acquired_order = 0, geometry_epoch = 0;
        uint32_t width = 0, height = 0, format = 0;
        bool callback = false, positive = false, published = false, invalid = false, direct_present = false;
    };

    class present_model {
    public:
        static constexpr size_t buffer_limit = 64, drawable_limit = 8;
        uint64_t session = 0;
        fault_kind fault = fault_kind::none;
        bool stopping = false;
        present_counters counters{};
        std::array<submission, buffer_limit> buffers{};
        std::array<acquisition, drawable_limit> drawables{};

    private:
        uint64_t serial = 0, clock = 0, last_published_frame = 0;
        presented_frame latest{};
        bool unread = false;
        uint64_t geometry_epoch = 1;
        uint32_t geometry_width = 0, geometry_height = 0, geometry_format = 0;

        uint64_t next() { return ++serial; }
        uint64_t event() { return ++clock; }

        bool used_by_drawable(uint64_t buffer_id) const {
            for (const auto& d : drawables) {
                if (d.id && d.final_buffer == buffer_id) return true;
            }
            return false;
        }

        bool protected_history(const submission& original, const operation& op) const {
            for (const auto& d : drawables) {
                if (!d.id) continue;
                if (d.source_texture != op.texture && !(!d.source_texture && op.type == operation::eof && op.order < d.acquired_order)) continue;

                const auto* final = buffer(d.final_buffer);
                if (!final) return true;
                int relation = compare(original, op.order, *final, d.copy_order);
                if (relation == 1) continue;
                if (relation == 2) return true;

                bool replaced = false;
                for (const auto& b : buffers) {
                    if (!b.id || !b.completed || !b.good) continue;
                    for (uint32_t i = 0; i < b.count; ++i) {
                        const auto& newer = b.operations[i];
                        if (newer.type == operation::eof && newer.texture == op.texture &&
                            (d.source_texture || newer.order < d.acquired_order) &&
                            compare(original, op.order, b, newer.order) == -1 &&
                            compare(b, newer.order, *final, d.copy_order) == -1) replaced = true;
                    }
                }

                if (!replaced) return true;
            }

            return false;
        }

        bool superseded(const submission& original, uint64_t texture) const {
            for (const auto& b : buffers) {
                if (!b.id || b.id == original.id || !b.completed || !b.good || !before(original, b)) continue;
                for (uint32_t n = 0; n < b.count; ++n) {
                    if (b.operations[n].type == operation::eof && b.operations[n].texture == texture) return true;
                }
            }
            return false;
        }

        bool reclaimable(const submission& b) const {
            if (!b.id || !b.completed || b.queue_call || used_by_drawable(b.id)) return false;
            if (stopping || fault != fault_kind::none) return true;

            for (uint32_t n = 0; n < b.count; ++n) {
                const auto& o = b.operations[n];
                if (o.type != operation::write && o.type != operation::eof) continue;
                if (o.type == operation::write && !watched(o.texture)) continue;
                if (protected_history(b, o) || !superseded(b, o.texture)) return false;
            }

            return true;
        }

        operation* add(uint64_t id, operation::kind type, uint64_t texture) {
            auto* b = buffer(id);
            if (!b || b->completed || stopping || fault != fault_kind::none) {
                ++counters.rejected;
                return nullptr;
            }

            // Consecutive writes to one texture share a record: no EOF or copy lies
            // between them, so they relate identically to every other operation.
            // Never coalesce across an EOF or copy; that could hide a source overwrite.
            if (type == operation::write) {
                for (uint32_t n = b->count; n > 0; --n) {
                    auto& previous = b->operations[n - 1];
                    if (previous.type != operation::write) break;
                    if (previous.texture == texture) return &previous;
                }
            }

            if (b->count == b->operations.size()) {
                fail(fault_kind::pool);
                return nullptr;
            }

            auto& o = b->operations[b->count++];
            o = {};
            o.type = type;
            o.order = event();
            o.texture = texture;
            return &o;
        }

        bool drawable_gpu_done(const acquisition& d) const {
            const auto* b = buffer(d.final_buffer);
            return !d.final_buffer || (b && b->completed);
        }

        // Ordering needs non-overlapping queue calls. A still unqueued buffer that
        // was created after another buffer's queue call ended cannot precede it.
        int compare(const submission& a, uint64_t event_a, const submission& b, uint64_t event_b) const {
            if (a.id == b.id) return event_a < event_b ? -1 : event_a > event_b ? 1 : 0;
            if (before(a, b)) return -1;
            if (before(b, a)) return 1;
            if (a.created > b.enqueue_end && b.enqueue_end && !a.enqueue_begin) return 1;
            if (b.created > a.enqueue_end && a.enqueue_end && !b.enqueue_begin) return -1;
            return 2;
        }

        bool candidate(const acquisition& d, presented_frame& result) const {
            const auto* final = buffer(d.final_buffer);
            if (!final || !final->completed || !final->good || !d.callback || !d.positive || d.invalid ||
                d.present_buffer != d.final_buffer || !d.copy_order || d.geometry_epoch != geometry_epoch) return false;

            const submission* source = nullptr;
            const operation* eof_op = nullptr;

            for (const auto& b : buffers) {
                if (!b.id) continue;
                for (uint32_t i = 0; i < b.count; ++i) {
                    const auto& o = b.operations[i];
                    if (o.type != operation::eof || o.geometry_epoch != d.geometry_epoch || o.order >= d.acquired_order ||
                        (d.source_texture && o.texture != d.source_texture)) continue;

                    int relation = compare(b, o.order, *final, d.copy_order);
                    if (relation == 2) return false;
                    if (relation != -1) continue;
                    if (!source) {
                        source = &b;
                        eof_op = &o;
                    } else {
                        relation = compare(*source, eof_op->order, b, o.order);
                        if (relation == 2) return false;
                        if (relation < 0) {
                            source = &b;
                            eof_op = &o;
                        }
                    }
                }
            }

            if (!source || !eof_op || !source->completed || !source->good || eof_op->marker.session != session ||
                (d.source_texture && (eof_op->marker.width != d.width || eof_op->marker.height != d.height))) return false;

            for (const auto& b : buffers) {
                if (!d.source_texture || !b.id) continue;
                for (uint32_t i = 0; i < b.count; ++i) {
                    const auto& o = b.operations[i];
                    if (o.type != operation::write || o.texture != d.source_texture) continue;

                    int after = compare(*source, eof_op->order, b, o.order);
                    int before_copy = compare(b, o.order, *final, d.copy_order);
                    if (after == 2 || before_copy == 2 || (after < 0 && before_copy < 0)) return false;
                }
            }

            const auto& k = eof_op->marker;
            result = {k.session, k.source_frame, k.generation, k.content_revision, k.restore_serial, 0, d.address, d.acquired_ns, d.presented_ns};
            return true;
        }

    public:
        explicit present_model(uint64_t value = 0) : session(value) {}

        static bool before(const submission& a, const submission& b) {
            return a.enqueue_begin && a.enqueue_end && b.enqueue_begin && b.enqueue_end && a.enqueue_end < b.enqueue_begin;
        }

        submission* buffer(uint64_t id) {
            for (auto& b : buffers) {
                if (id && b.id == id) return &b;
            }
            return nullptr;
        }

        const submission* buffer(uint64_t id) const {
            for (const auto& b : buffers) {
                if (id && b.id == id) return &b;
            }
            return nullptr;
        }

        acquisition* drawable(uint64_t id) {
            for (auto& d : drawables) {
                if (id && d.id == id) return &d;
            }
            return nullptr;
        }

        const acquisition* drawable(uint64_t id) const {
            for (const auto& d : drawables) {
                if (id && d.id == id) return &d;
            }
            return nullptr;
        }

        size_t live_buffers() const {
            size_t n = 0;
            for (const auto& b : buffers) n += b.id != 0;
            return n;
        }

        size_t live_drawables() const {
            size_t n = 0;
            for (const auto& d : drawables) n += d.id != 0;
            return n;
        }

        bool watched(uint64_t texture) const {
            for (const auto& b : buffers) {
                if (!b.id) continue;
                for (uint32_t n = 0; n < b.count; ++n) {
                    if (b.operations[n].texture == texture && b.operations[n].type == operation::eof) return true;
                }
            }
            return false;
        }

        void fail(fault_kind why) {
            if (fault == fault_kind::none) fault = why;
            ++counters.rejected;
            if (why == fault_kind::pool) ++counters.pool_pressure;
            if (why == fault_kind::unknown_route) ++counters.unknown_routes;
            if (why == fault_kind::class_conflict) ++counters.class_conflicts;
            if (why == fault_kind::queue_conflict) ++counters.queue_conflicts;

            for (auto& d : drawables) {
                if (d.id) d.invalid = true;
            }

            unread = false;
        }

        void sweep() {
            for (auto& d : drawables) {
                if (d.id && drawable_gpu_done(d) &&
                    (stopping || fault != fault_kind::none || d.invalid || d.published || (d.callback && !d.positive))) d = {};
            }

            // Retired acquisitions release the buffers they depended on.
            for (auto& b : buffers) {
                if (reclaimable(b)) b = {};
            }
        }

        uint64_t register_buffer(uint64_t object, bool fresh, uint32_t status) {
            if (!object || stopping || fault != fault_kind::none) return 0;

            bool completed_predecessor = false;
            for (auto& b : buffers) {
                if (!b.id || b.object != object) continue;
                if (b.completed) {
                    completed_predecessor = true;
                    continue;
                }
                if (fresh) {
                    fail(fault_kind::identity);
                    return 0;
                }
                if (status == 0 && b.enqueue_end) {
                    fail(fault_kind::identity);
                    return 0;
                }
                if (status > 1) {
                    ++counters.rejected;
                    return 0;
                }

                return b.id;
            }

            // Metal reuses buffer objects after completion. A status 0 object gets a
            // new serial; the retained EOF and copy history is never reset.
            if (status > 1 || (completed_predecessor && status != 0)) {
                ++counters.rejected;
                return 0;
            }

            sweep();
            for (auto& b : buffers) {
                if (b.id) continue;
                b = {};
                b.id = next();
                b.object = object;
                b.created = event();
                return b.id;
            }

            fail(fault_kind::pool);
            return 0;
        }

        bool begin_queue(uint64_t id, bool explicit_enqueue, uint32_t status) {
            (void)explicit_enqueue;
            auto* b = buffer(id);
            if (!b) return false;

            if (b->completed || b->queue_call) {
                fail(fault_kind::queue_conflict);
                return false;
            }
            if (status != 0) {
                if (!b->enqueue_end) ++counters.rejected;
                return false;
            }
            if (b->enqueue_begin) {
                fail(fault_kind::queue_conflict);
                return false;
            }

            b->queue_call = true;
            b->enqueue_begin = event();
            return true;
        }

        void end_queue(uint64_t id, bool began, uint32_t status) {
            auto* b = buffer(id);
            if (!b || !began) return;

            b->queue_call = false;
            b->enqueue_end = event();
            if (status < 1) {
                fail(fault_kind::queue_conflict);
                return;
            }

            // Metal allows concurrent submissions on a queue. Overlapping calls do not
            // establish an order and compare() rejects a frame that depends on one,
            // but unrelated copy work must not poison later, properly ordered frames.
            evaluate();
        }

        void complete(uint64_t id, uint64_t object, bool good) {
            auto* b = buffer(id);
            if (!b || b->object != object || b->completed) {
                ++counters.stale_callbacks;
                return;
            }

            b->completed = true;
            b->good = good;
            ++counters.gpu_completed;
            if (!good) fail(fault_kind::identity);

            evaluate();
            sweep();
        }

        bool write(uint64_t id, uint64_t texture) {
            if (!texture) return true;
            if (!add(id, operation::write, texture)) return false;
            ++counters.source_writes;
            return true;
        }

        bool eof(uint64_t id, uint64_t texture, const session_native_frame& marker) {
            if (!texture || marker.size != 64 || marker.version != 1 || marker.session != session || !marker.source_frame ||
                !marker.width || !marker.height || marker.flags || marker.reserved) {
                ++counters.rejected;
                return false;
            }

            auto* o = add(id, operation::eof, texture);
            if (!o) return false;

            o->marker = marker;
            o->geometry_epoch = geometry_epoch;
            ++counters.source_epochs;
            return true;
        }

        void set_geometry(uint32_t width, uint32_t height, uint32_t format) {
            if (geometry_width && (width != geometry_width || height != geometry_height || format != geometry_format)) {
                ++geometry_epoch;
                unread = false;
                for (auto& d : drawables) {
                    if (d.id) d.invalid = true;
                }
            }

            geometry_width = width;
            geometry_height = height;
            geometry_format = format;
        }

        uint64_t acquire(uint64_t object, uint64_t texture, uint32_t width, uint32_t height, uint32_t format, uint64_t ns, uint64_t address = 0) {
            if (!object || !texture || !width || !height || !ns || stopping || fault != fault_kind::none) return 0;

            set_geometry(width, height, format);
            sweep();

            for (auto& d : drawables) {
                if (!d.id || d.object != object) continue;
                if (!drawable_gpu_done(d)) {
                    fail(fault_kind::identity);
                    return 0;
                }

                // Reacquisition expires the previous use. A late handler still holds
                // the old serial and cannot hit this one.
                d.invalid = true;
                d = {};
            }

            acquisition* slot = nullptr;
            for (auto& d : drawables) {
                if (!d.id) {
                    slot = &d;
                    break;
                }
            }

            if (!slot) {
                for (auto& d : drawables) {
                    if (drawable_gpu_done(d) && (!slot || d.id < slot->id)) slot = &d;
                }
            }

            if (!slot) {
                fail(fault_kind::pool);
                return 0;
            }

            *slot = {};
            slot->id = next();
            slot->object = object;
            slot->address = address ? address : object;
            slot->texture = texture;
            slot->width = width;
            slot->height = height;
            slot->format = format;
            slot->acquired_ns = ns;
            slot->acquired_order = event();
            slot->geometry_epoch = geometry_epoch;
            return slot->id;
        }

        bool full_copy(uint64_t buffer_id, uint64_t source, uint64_t destination, uint64_t acquisition_id,
            uint32_t width, uint32_t height, uint32_t format, bool exact) {
            auto* d = drawable(acquisition_id);
            if (!d || d->texture != destination || d->final_buffer || !exact ||
                d->width != width || d->height != height || d->format != format || !watched(source)) {
                if (d) d->invalid = true;
                ++counters.rejected;
                return false;
            }

            auto* o = add(buffer_id, operation::copy, source);
            if (!o) return false;

            o->destination = destination;
            o->drawable = acquisition_id;
            d->final_buffer = buffer_id;
            d->source_texture = source;
            d->copy_order = o->order;
            ++counters.copies;
            return true;
        }

        // Any attachment, resolve or blit write to the drawable counts as target
        // work. Nothing is inferred about the shader's sampled source.
        bool target_write(uint64_t buffer_id, uint64_t destination, uint64_t acquisition_id) {
            auto* d = drawable(acquisition_id);
            auto* b = buffer(buffer_id);
            if (!b || b->completed || !d || d->invalid || d->texture != destination || d->present_buffer) {
                ++counters.rejected;
                return false;
            }

            if (d->final_buffer && d->final_buffer != buffer_id) {
                d->invalid = true;
                ++counters.rejected;
                return false;
            }

            if (d->final_buffer == buffer_id && !d->source_texture) {
                // Repeated passes on the same target only move the last write boundary.
                d->copy_order = event();
                return true;
            }

            auto* o = add(buffer_id, operation::copy, 0);
            if (!o) return false;

            o->destination = destination;
            o->drawable = acquisition_id;
            d->final_buffer = buffer_id;
            d->source_texture = 0;
            d->copy_order = o->order;
            return true;
        }

        void present(uint64_t acquisition_id, uint64_t object, uint64_t scheduled_buffer, bool direct = false) {
            auto* d = drawable(acquisition_id);
            if (!d || d->object != object) {
                ++counters.stale_callbacks;
                return;
            }

            // presentDrawable: may be implemented with a scheduled handler that calls
            // drawable.present. That is the same request, not a second one.
            if (!direct && d->direct_present && d->present_buffer == scheduled_buffer && scheduled_buffer) return;
            ++counters.present_calls;

            if (!scheduled_buffer || scheduled_buffer != d->final_buffer || d->present_buffer) {
                d->invalid = true;
                ++counters.rejected;
                return;
            }

            d->present_buffer = scheduled_buffer;
            d->direct_present = direct;
            evaluate();
        }

        void presented(uint64_t acquisition_id, uint64_t object, double actual_time, uint64_t ns) {
            auto* d = drawable(acquisition_id);
            if (!d || d->object != object) {
                ++counters.stale_callbacks;
                return;
            }

            if (d->callback) {
                d->invalid = true;
                ++counters.rejected;
                return;
            }

            d->callback = true;
            d->positive = std::isfinite(actual_time) && actual_time > 0 && ns >= d->acquired_ns;
            d->presented_ns = ns;

            if (d->positive) {
                ++counters.presented_positive;
            } else {
                ++counters.presented_zero;
                d->invalid = true;
            }

            evaluate();
            sweep();
        }

        void evaluate() {
            if (stopping || fault != fault_kind::none) return;

            for (auto& d : drawables) {
                if (!d.id || d.published || d.invalid) continue;

                presented_frame found;
                if (!candidate(d, found)) continue;
                d.published = true;
                if (found.frame <= last_published_frame) continue;
                found.serial = next();
                latest = found;
                last_published_frame = found.frame;
                unread = true;
                ++counters.published;
            }
        }

        bool poll(presented_frame& result) {
            evaluate();
            if (!unread || stopping || fault != fault_kind::none) return false;

            result = latest;
            unread = false;
            return true;
        }

        void stop() {
            stopping = true;
            unread = false;

            for (auto& d : drawables) {
                if (d.id) d.invalid = true;
            }

            sweep();
        }

        // Only for our own private copy buffer that was never enqueued or committed.
        // Source EOF history is never discarded.
        bool discard_unsubmitted_copy(uint64_t id, uint64_t object) {
            auto* b = buffer(id);
            if (!b || b->object != object || b->completed || b->enqueue_begin || b->enqueue_end || b->queue_call || used_by_drawable(id)) return false;

            for (uint32_t i = 0; i < b->count; ++i) {
                if (b->operations[i].type != operation::write || watched(b->operations[i].texture)) return false;
            }

            *b = {};
            return true;
        }

        bool gpu_idle() const {
            for (const auto& b : buffers) {
                if (b.id && !b.completed) return false;
            }
            return true;
        }
    };

}
