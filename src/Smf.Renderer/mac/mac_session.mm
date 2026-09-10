#include "metal_owner.h"
#include "ownership.h"
#include "packet_policy.h"
#include "present_observer.h"
#include "present_recovery.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <type_traits>

namespace mac {
    namespace {

        constexpr int malformed = -201;
        constexpr int wrong_owner = -202;
        constexpr int gpu_fault = -203;
        constexpr int unavailable = -204;

        // Command operations, in ABI order.
        enum operation : uint32_t {
            op_prepare,
            op_invalidate,
            op_replace,
            op_activate,
            op_retire_generation,
            op_release_main,
            op_restore_routing,
            op_await_native,
            op_detach,
            op_retire_session,
            op_release_session,
            op_stop
        };

        enum slot_state { slot_free, slot_source, slot_ready, slot_worker, slot_retiring, slot_quarantined };

        // One captured source frame: our copies of the textures Unity handed the render callback.
        struct source_slot {
            slot_state state = slot_free;
            uint64_t generation = 0, content = 0, frame = 0, source_serial = 0;
            bool sealed = false, abandoned = false, has_world = false;
            image_layer base{}, world{}, hud{}, cache{};
            session_pose pose{};
            affine actual{};
            __strong id<MTLTexture> inputs[4]{nil, nil, nil, nil};
            std::shared_ptr<source_completion> base_copy, seal_copy, cache_dependency;
        };

        struct generation_state {
            session_generation_status status{};
            camera_model model{};
            session_cache cache_packet{};
            image_layer cache{};
            std::shared_ptr<source_completion> cache_copy;
            uint64_t target_queued_frame = 0;
            bool retiring = false;
        };

        // One queued render event. Unity calls back with the ticket pointer and its token.
        struct dispatch_ticket {
            int token = 0, copy_probe_index = 0;
            bool cancelled = false;
            uint32_t kind = 0; // 1 pre-GUI, 2 end of frame, 3 native marker
            mac_target_probe_observation probe{};
            mac_source_target target{};
            mac_source_base base{};
            session_pre_gui pre{};
            session_frame frame{};
            session_native_frame marker{};
            __strong id<MTLTexture> inputs[4]{nil, nil, nil, nil};
        };

        // The draw the worker is currently submitting.
        struct draw_work {
            int slot = -1;
            bool hidden = false, selection_allowed = false;
            uint64_t session = 0;
            image_layer base{}, world{}, hud{}, cache{};
            affine desired{};
            camera_model model{};
            uint64_t bridge_epoch = 0;
            int32_t map = -1;
            std::shared_ptr<draw_completion> receipt;
        };

        // Context and all callback captures survive until process teardown. Sources and
        // generations are bounded; no detached thread or static destructor unloads code.
        std::mutex& gate = *new std::mutex();
        std::array<dispatch_ticket, 32>& tickets = *new std::array<dispatch_ticket, 32>();
        missed_callbacks<32>& missed = *new missed_callbacks<32>();
        std::array<source_slot, 6>& slots = *new std::array<source_slot, 6>();
        std::array<generation_state, 2>& generations = *new std::array<generation_state, 2>();
        window_owner& window = *new window_owner();
        metal_worker& compositor = *new metal_worker();
        std::thread& worker = *new std::thread();
        std::atomic<bool> worker_exited{false}, process_exit{false};
        std::atomic<IUnityInterfaces*> interfaces{nullptr};
        std::atomic<uint64_t> loads{0};

        session_status state{472, 1};
        session_command pending{};
        session_ack acknowledgement{};
        mac_source_base staged_base{};
        __strong id<MTLDevice> source_device = nil;
        source_target_stage source_target{};
        native_target_stage native_target{};
        uint64_t native_target_queued_frame = 0;
        draw_work work{};
        std::shared_ptr<draw_completion> latest_visible;
        std::shared_ptr<presentation_count> presentations;
        std::shared_ptr<activation_attempt> activation;

        uint64_t token_floor = 0, session_floor = 0, show_transaction = 0, show_completed_ns = 0;
        uint64_t generation_floor = 0, restore_after_frame = 0;
        int visible = -1;
        bool started = false, worker_ready = false, source_released = false, joined = false;
        bool routing_fence = false, stop_worker = false;
        bool current_target_mode = false, target_probe_mode = false, copy_probe_mode = false;
        present_observer::recovery observer_restore;
        present_observer::source_rejection source_before_restore{};
        mac_target_probe_diagnostic target_diagnostic{672, 1};
        std::array<mac_target_probe_observation, 2> staged_target{};

        bool on_main() {
            return pthread_main_np() && state.main_thread == native_thread();
        }

        generation_state* find_generation(uint64_t id) {
            for (auto& g : generations) {
                if (id && g.status.generation == id) return &g;
            }
            return nullptr;
        }

        bool is_prepare(uint32_t op) {
            return op == op_prepare || op == op_replace;
        }

        bool has_dispatches(uint64_t generation = 0) {
            for (auto& d : tickets) {
                if (d.token && (!generation || d.pre.generation == generation || d.frame.generation == generation || d.marker.generation == generation)) return true;
            }
            return false;
        }

        bool has_leases(uint64_t generation = 0) {
            for (auto& s : slots) {
                if (s.state != slot_free && (!generation || s.generation == generation)) return true;
            }
            return false;
        }

        texture_budget budget() {
            texture_budget result;
            auto add = [&](const image_layer& l) { result.add((uint64_t)(__bridge void*)l.texture, l.width, l.height); };

            for (auto& s : slots) {
                add(s.base);
                add(s.world);
                add(s.hud);
                add(s.cache);
            }

            for (auto& g : generations) add(g.cache);
            return result;
        }

        uint64_t allocation(const image_layer& l, uint32_t width, uint32_t height) {
            return l.texture && l.width == width && l.height == height ? 0 : uint64_t(width) * height * 4;
        }

        // Layer pool plus one hidden preparation target.
        uint64_t display_reserve() {
            return uint64_t(window.width) * window.height * 4 * 4;
        }

        // Keep the allocated textures for reuse, drop everything else.
        void recycle(source_slot& s) {
            image_layer base = s.base;
            image_layer world = s.world;
            image_layer hud = s.hud;

            s = source_slot{};

            s.base = base;
            s.world = world;
            s.hud = hud;
        }

        void ack(uint32_t evidence, int result = 0, uint32_t disposition = 1, uint64_t frame = 0) {
            acknowledgement = {80, 1, pending.session, pending.serial, pending.generation, pending.content_revision, frame,
                pending.operation, evidence, result, disposition, state.worker_completed, native_now()};

            state.last_ack_serial = pending.serial;
            pending = {};
        }

        void fail(int error) {
            if (state.result >= 0) state.result = error;
            state.stage = 6;
            routing_fence = true;
            activation.reset();

            if (pending.serial) ack(0, state.result, 3);
        }

        bool completed(const std::shared_ptr<source_completion>& c) {
            return c && c->completed();
        }

        bool source_done(const source_slot& s) {
            return completed(s.base_copy) && (!s.sealed || completed(s.seal_copy)) && (!s.cache_dependency || completed(s.cache_dependency));
        }

        // The newest sealed, usable slot of a generation.
        int best_slot(uint64_t generation) {
            int result = -1;
            for (int i = 0; i < 6; ++i) {
                auto& s = slots[i];
                if ((s.state == slot_ready || s.state == slot_worker) && s.sealed && !s.abandoned && s.generation == generation &&
                    (result < 0 || s.frame > slots[result].frame)) result = i;
            }
            return result;
        }

        bool native_observer_available() {
            if (!current_target_mode) return original_observer_available();
            return !observer_restore.replacing() && observer_restore.state() != present_observer::recovery::failed && present_observer::available();
        }

        bool read_original_frame(original_frame& result) {
            if (!current_target_mode) return poll_original_frame(result);

            present_observer::presented_frame frame;
            if (!present_observer::poll(frame) || !observer_restore.allows(frame, state.content_fence)) return false;

            result = {frame.session, frame.frame, frame.generation, frame.content, frame.restore,
                frame.serial, frame.drawable, frame.acquired_ns, frame.presented_ns};
            return true;
        }

        void pump_observer_restore() {
            if (!current_target_mode || !observer_restore.replacing()) return;

            observer_restore.pump([&] { return present_observer::remove(state.session); },
                [&] { return present_observer::install(state.session, window.original); });
            if (observer_restore.error() < 0) fail(observer_restore.error());
        }

        void poll_original() {
            if (current_target_mode && state.result >= 0) {
                const int fault = present_observer::failure(state.session);
                if (fault < 0) fail(fault);
            }

            original_frame frame;
            if (!read_original_frame(frame) || frame.session != state.session || !frame.serial || !frame.drawable || !frame.presented_ns) return;

            state.flags |= 2;
            state.native_present_serial = frame.serial;
            state.native_backbuffer = frame.drawable;
            state.native_submitted_frame = state.native_reveal_frame = frame.frame;
            state.native_submitted_generation = frame.generation;
            state.native_submitted_content = frame.content;
            state.native_submitted_restore_serial = frame.restore;
            if (window.visible && original_behind_overlay(state.session, frame.session, frame.acquired_ns, show_completed_ns)) state.flags |= 16;

            if (pending.serial && pending.operation == op_await_native && frame.restore == state.operation_fence &&
                (current_target_mode
                    ? (frame.generation == 0 && frame.content == state.content_fence && frame.content >= pending.content_revision)
                    : (frame.generation == pending.generation && frame.content == pending.content_revision)) &&
                frame.frame > pending.after_frame) ack(128, 0, 1, frame.frame);
        }

        void retire_sources() {
            // Only the render callback releases retained source textures. A cancelled
            // ticket stays on the ledger until this ordered pump consumes it.
            for (size_t i = 0; i < tickets.size(); ++i) {
                auto& d = tickets[i];
                if (d.token && missed.contains(i, d.token)) d.cancelled = true;

                if (d.token && d.cancelled) {
                    cancel_copy_probe(d.copy_probe_index);
                    if (d.kind == 2) {
                        for (auto& s : slots) {
                            if (s.generation == d.frame.generation && s.frame == d.frame.source_frame) s.abandoned = true;
                        }
                    }

                    d = dispatch_ticket{};
                    --state.queued_callbacks;
                }
            }

            for (int i = 0; i < 6; ++i) {
                auto& s = slots[i];
                if (s.state == slot_free) continue;

                if (s.state == slot_quarantined) {
                    if (!source_done(s)) continue;
                    s.state = slot_retiring;
                    s.abandoned = true;
                }

                auto* g = find_generation(s.generation);

                if (source_done(s)) {
                    state.completion_thread = s.seal_copy ? s.seal_copy->thread : s.base_copy->thread;
                    for (auto& input : s.inputs) input = nil;

                    bool failed = !s.base_copy->retired_without_failure() || (s.sealed && !s.seal_copy->retired_without_failure()) ||
                        (s.cache_dependency && !s.cache_dependency->retired_without_failure());
                    if (failed) {
                        s.state = slot_retiring;
                        s.abandoned = true;
                        fail(gpu_fault);
                    }

                    const bool usable = s.base_copy->succeeded() && (!s.sealed || s.seal_copy->succeeded()) &&
                        (!s.cache_dependency || s.cache_dependency->succeeded());
                    // A never-submitted copy can retire its producer lease, but cannot
                    // publish pixels even if its caller left the slot in slot_source.
                    if (!usable) {
                        s.state = slot_retiring;
                        s.abandoned = true;
                    }

                    if (s.state == slot_source && s.sealed) {
                        s.state = slot_ready;
                        state.source_completed++;
                        if (g) {
                            g->status.completed_commit = std::max(g->status.completed_commit, s.source_serial);
                            g->status.last_source_frame = std::max(g->status.last_source_frame, s.frame);
                            g->status.frames++;
                        }
                    }
                }

                bool superseded = s.abandoned || !g || g->retiring || ((s.state == slot_ready || s.state == slot_worker) && best_slot(s.generation) != i);
                if (superseded && source_may_retire(source_done(s), i == visible, i == work.slot)) {
                    if (!g || g->retiring) s = source_slot{};
                    else recycle(s);
                }
            }

            for (auto& g : generations) {
                if (g.retiring && !has_leases(g.status.generation) && !has_dispatches(g.status.generation)) {
                    g.cache = {};
                    g.cache_copy.reset();
                    g.status.state = 5;
                    g.status.retired_serial = g.status.retire_requested;
                }
            }

            if (pending.serial && pending.operation == op_retire_generation) {
                auto* g = find_generation(pending.generation);
                if (!g || g->status.state == 5) ack(512);
            }

            if (pending.serial && pending.operation == op_retire_session && !has_leases() && !has_dispatches() && !copy_probe_in_flight()) {
                for (auto& s : slots) s = source_slot{};
                source_device = nil;
                source_released = true;
                ack(512);
            }
        }

        void process_draw_completion() {
            if (work.slot < 0 || !work.receipt || !work.receipt->completed.load(std::memory_order_acquire)) return;

            auto r = work.receipt;
            auto* g = find_generation(r->generation);

            if (r->result.load() < 0) {
                slots[work.slot].state = slot_retiring;
                slots[work.slot].abandoned = true;
                fail(gpu_fault);
                work = {};
                return;
            }

            state.worker_completed = r->serial;
            if (work.hidden) {
                if (g) {
                    g->status.prepared_frame = r->frame;
                    g->status.state = 2;
                    g->status.completed_commit = r->serial;
                }
                if (pending.serial && is_prepare(pending.operation) && pending.generation == r->generation && r->frame > pending.after_frame) ack(2, 0, 1, r->frame);
            } else {
                if (visible >= 0 && visible != work.slot) slots[visible].abandoned = true;
                visible = work.slot;
                latest_visible = r;
            }

            work = {};
        }

        void run_worker();

        void pump_main() {
            if (!on_main()) return;

            pump_observer_restore();

            if (!target_probe_mode && !worker_ready && source_device && pending.serial && is_prepare(pending.operation) && !worker.joinable()) {
                auto* g = find_generation(pending.generation);
                int configured = g ? window.configure(source_device, g->status.width, g->status.height) : malformed;

                if (configured < 0) {
                    fail(configured);
                    return;
                }

                if (configured == 0) {
                    state.worker_state = 1;
                    worker_exited = false;
                    // No AppKit calls run inside the worker.
                    worker = std::thread(run_worker);
                }
            }

            auto* activating = pending.serial && pending.operation == op_activate ? find_generation(pending.generation) : nullptr;
            if (!target_probe_mode && activating && activating->status.state == 2 && activating->status.prepared_frame &&
                activating->status.content_revision == pending.content_revision &&
                activation_ready(native_observer_available(), window.visible, state.active_generation, routing_fence) && !activating->retiring) {
                // Hidden preparation is complete. The initial layer has no drawable
                // contents and a transparent background, so showing it keeps the
                // original UI until our first full draw. That draw waits until this
                // show transaction completes and its activation attempt exists.
                if (!window.visible) {
                    show_transaction = window.show(true);
                    show_completed_ns = 0;
                    state.flags &= ~16u;
                }

                if (show_transaction && window.transaction_completed.load(std::memory_order_acquire) >= show_transaction && !show_completed_ns) show_completed_ns = native_now();

                if (window.visible && show_completed_ns && !activation) {
                    activation = std::make_shared<activation_attempt>(state.session, pending.serial, pending.generation, pending.content_revision, show_completed_ns);
                }

                // Preparation already confirmed the full original native UI. Activation
                // confirms our own displayed replacement; an original frame presented
                // behind the overlay stays diagnostic only and is required again on restore.
                if (activation && window.visible && show_completed_ns &&
                    activation->matches(state.session, pending.serial, pending.generation, pending.content_revision, show_completed_ns)) {
                    const uint64_t frame = activation->frame();
                    if (frame) {
                        state.active_generation = pending.generation;
                        state.active_frame = frame;
                        if (auto* g = find_generation(pending.generation)) g->status.state = 3;

                        // Captured active frames have no complete native EOF. Stop
                        // recording observer write history until a restore replaces
                        // this scope; retained hooks and GPU leases drain normally.
                        if (current_target_mode) present_observer::stop(state.session);
                        ack(4, 0, 1, frame);
                        activation.reset();
                    }
                }
            }

            if (pending.serial && pending.operation == op_detach && work.slot < 0) {
                if (window.visible) {
                    show_transaction = window.show(false);
                    show_completed_ns = 0;
                }

                if (!window.visible && (!show_transaction || window.transaction_completed.load(std::memory_order_acquire) >= show_transaction)) {
                    if (visible >= 0) slots[visible].abandoned = true;
                    visible = -1;
                    latest_visible.reset();
                    activation.reset();
                    state.active_generation = state.active_frame = 0;
                    ack(256);
                }
            }
        }

        // Picks the next draw while the gate is held. Returns false when nothing is ready.
        bool pick_draw(draw_work& next) {
            if (!worker_may_draw(work.slot < 0, state.result >= 0, pending.serial && pending.operation == op_detach)) return false;

            uint64_t target_generation = state.active_generation;
            bool hidden = false;

            if (pending.serial && is_prepare(pending.operation)) {
                target_generation = pending.generation;
                hidden = true;
            } else if (pending.serial && pending.operation == op_activate) {
                const bool attempt_ready = activation && window.visible && show_completed_ns &&
                    activation->matches(state.session, pending.serial, pending.generation, pending.content_revision, show_completed_ns);
                const bool observed = activation_ready(native_observer_available(), window.visible, state.active_generation, routing_fence);
                target_generation = activation_draw_generation(state.active_generation, pending.generation, observed, attempt_ready);
            }

            int index = best_slot(target_generation);
            if (index < 0) return false;
            auto& s = slots[index];
            auto* g = find_generation(target_generation);
            if (!g) return false;

            s.state = slot_worker;
            next.slot = index;
            next.hidden = hidden;
            next.base = s.base;
            next.world = s.has_world ? s.world : image_layer{};
            next.hud = s.hud;
            next.cache = s.cache;
            next.desired = s.actual;
            next.model = g->model;
            next.bridge_epoch = s.pose.bridge_epoch;
            next.map = s.pose.map_id;
            next.session = state.session;
            next.selection_allowed = !hidden && !routing_fence && !source_released && s.content == state.content_fence;
            next.receipt = std::make_shared<draw_completion>();
            next.receipt->serial = state.worker_commit + 1;
            next.receipt->frame = s.frame;
            next.receipt->generation = s.generation;
            next.receipt->content = s.content;
            next.receipt->attempt = next.hidden ? nullptr : activation;

            if (!next.hidden) {
                if (!presentations || presentations->session != state.session || presentations->generation != s.generation) {
                    presentations = std::make_shared<presentation_count>(state.session, s.generation);
                }
                next.receipt->presentation = presentations;
            }

            work = next;
            return true;
        }

        // Draws one picked slot. The slot is reserved in work while the gate is released;
        // neither OS input queries, nextDrawable nor GPU submission holds the gate.
        void draw_picked(draw_work& next) {
            const auto selection_snapshot = compositor.selection.latch();
            bool pose_valid = true;
            double x = 0, y = 0;
            bool middle = false, focus = false, left = false, pointer = false;

            if (!next.hidden && next.world.texture) {
                pointer = camera_control::observe(x, y, middle, focus, left);

                smf_bridge_desired desired{};
                if (camera_bridge::worker_prepare(focus, pointer, x, y, middle, desired) == 0) {
                    pose_valid = desired.epoch == next.bridge_epoch && desired.map_id == next.map &&
                        next.model.root(desired.x, desired.z, desired.projection_half_height, next.desired);
                }
            } else {
                camera_control::clear();
            }

            auto selection_geometry = compositor.selection.read(selection_snapshot, next.session, next.receipt->content, next.map,
                next.desired, focus && next.selection_allowed, pointer, left, x, y);
            if (!next.selection_allowed || !next.world.texture) selection_geometry.visible = false;
            int result = pose_valid ? compositor.draw(next.base, next.world, next.hud, next.cache, next.desired, next.receipt, next.hidden, selection_geometry) : 1;

            const bool submitted = next.receipt->submission_attempted.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(gate);
                if (result == 0) {
                    state.worker_commit = next.receipt->serial;
                    if (!next.hidden) camera_bridge::worker_committed(0);
                } else {
                    if (!submitted) work = {};
                    if (result < 0) fail(result);
                }
            }

            // A resizing drawable pool or a not yet adopted camera pose can be
            // temporarily unavailable. Retry without spinning or holding the
            // main-thread gate; source age is never a reason to stop drawing.
            if (result > 0 && !submitted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        void run_worker() {
            @autoreleasepool {
                __strong CAMetalLayer* target = nil;
                {
                    std::lock_guard<std::mutex> lock(gate);
                    state.worker_thread = native_thread();
                    target = window.layer;
                }

                int created = process_exit.load() ? unavailable : compositor.create(target);

                {
                    std::lock_guard<std::mutex> lock(gate);
                    if (created < 0) {
                        compositor.release();
                        fail(unavailable);
                        worker_exited = true;
                        return;
                    }

                    worker_ready = true;
                    state.worker_state = 2;
                    camera_bridge::worker_ready(native_thread(), 1000000000);
                }

                for (;;) {
                    draw_work next{};
                    bool picked = false;

                    {
                        std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
                        if (lock) {
                            process_draw_completion();
                            poll_original();

                            // Activation waits for the worker to observe the content fence,
                            // including the initial generation. Finish any older submission
                            // before acknowledging; Invalidate is not sent on first enable.
                            if (work.slot < 0) state.content_acknowledged = state.content_fence;

                            if ((process_exit.load() || stop_worker) && work.slot < 0) {
                                camera_bridge::worker_removed();
                                compositor.release();
                                worker_ready = false;
                                state.worker_state = 3;
                                worker_exited.store(true, std::memory_order_release);
                                return;
                            }

                            picked = pick_draw(next);
                        }
                    }

                    if (picked) draw_picked(next);
                    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        void pre_gui(dispatch_ticket& d, IUnityGraphicsMetal* metal, IUnityGraphicsMetalV2* copies_api) {
            auto& p = d.pre;
            if (target_probe_mode) return;
            if (routing_fence || source_released || state.result < 0) return;
            auto* g = find_generation(p.generation);
            if (!g || g->retiring || p.session != state.session || p.content_revision != state.content_fence || p.content_revision != g->status.content_revision ||
                p.width != g->status.width || p.height != g->status.height || p.flags != g->status.flags || p.source_frame <= g->status.last_source_frame) return;

            id<MTLDevice> device = metal->MetalDevice();
            resolved_source_target target;

            if (current_target_mode) {
                if (!source_target_matches(d.target, p)) {
                    fail(malformed);
                    return;
                }

                if (resolve_source_target(metal, d.target, target) < 0) {
                    state.dropped_frames++;
                    fail(unavailable);
                    return;
                }
            }

            id<MTLTexture> resolved = current_target_mode ? target.texture
                : metal->TextureFromRenderBuffer((UnityRenderBuffer)(uintptr_t)d.base.native_render_buffer);
            if (!resolved) {
                state.dropped_frames++;
                return;
            }

            if (!device || (!current_target_mode && resolved != d.inputs[0]) ||
                !texture_valid(resolved, device, p.width, p.height, current_target_mode ? 0 : d.base.pixel_format)) {
                NSLog(@"SMF PreGui unavailable site=resolved-validation session=%llu frame=%llu currentTarget=%d device=%p resolved=%p requested=%ux%u",
                    (unsigned long long)state.session, (unsigned long long)p.source_frame, current_target_mode,
                    (__bridge void*)device, (__bridge void*)resolved, p.width, p.height);
                fail(unavailable);
                return;
            }

            if (source_device && source_device != device) {
                NSLog(@"SMF PreGui unavailable site=source-device-changed session=%llu frame=%llu old=%p new=%p",
                    (unsigned long long)state.session, (unsigned long long)p.source_frame, (__bridge void*)source_device, (__bridge void*)device);
                fail(unavailable);
                return;
            }

            source_device = device;
            for (auto& old : slots) {
                if (old.state == slot_source && old.generation == p.generation && !old.sealed) old.abandoned = true;
            }

            retire_sources();

            source_slot* s = nullptr;
            for (auto& candidate : slots) {
                if (candidate.state == slot_free) {
                    s = &candidate;
                    break;
                }
            }
            if (!s) {
                state.dropped_frames++;
                return;
            }

            if (!budget().allows(allocation(s->base, p.width, p.height), display_reserve())) {
                state.dropped_frames++;
                return;
            }

            s->state = slot_source;
            s->generation = p.generation;
            s->content = p.content_revision;
            s->frame = p.source_frame;

            // Native Metal target rows follow its raster origin; owned Unity RTs keep
            // their explicit source-base orientation. Pixel parity is still a runtime gate.
            s->base.width = p.width;
            s->base.height = p.height;
            s->base.flip = current_target_mode ? false : !(d.base.flags & 1);
            s->inputs[0] = resolved;

            copy_pair copy{resolved, &s->base};
            int encoded = encode_copies(copies_api, &copy, 1, s->base_copy, target.command);
            if (encoded > 0) {
                s->abandoned = true;
                s->state = slot_retiring;
                state.dropped_frames++;
                return;
            }

            if (encoded < 0) {
                // Without a completion record no copy was encoded.
                if (s->base_copy) s->state = slot_quarantined;
                else recycle(*s);
                fail(gpu_fault);
            }
        }

        void seal(dispatch_ticket& d, IUnityGraphicsMetalV2* copies_api) {
            auto& f = d.frame;
            if (target_probe_mode) return;
            if (routing_fence || source_released || state.result < 0) return;
            auto* g = find_generation(f.generation);
            if (!g || g->retiring || f.session != state.session || f.content_revision != state.content_fence || f.content_revision != g->status.content_revision) return;

            source_slot* s = nullptr;
            for (auto& item : slots) {
                if (item.state == slot_source && !item.abandoned && item.generation == f.generation && item.content == f.content_revision && item.frame == f.source_frame) {
                    s = &item;
                    break;
                }
            }

            if (!s) {
                state.dropped_frames++;
                return;
            }

            uint64_t extra = allocation(s->hud, s->base.width, s->base.height);
            if (f.flags & 1) {
                extra += allocation(s->world, s->base.width, s->base.height);
                if (f.cache.serial != g->cache_packet.serial || !g->cache.texture) extra += uint64_t(f.cache.width) * f.cache.height * 4;
            }

            if (!budget().allows(extra, display_reserve())) {
                s->abandoned = true;
                state.dropped_frames++;
                return;
            }

            bool okay = valid_cache(f, g->cache_packet);
            s->has_world = (f.flags & 1) != 0;
            s->hud.width = s->base.width;
            s->hud.height = s->base.height;
            s->hud.flip = (f.flags & 16) != 0;
            s->inputs[1] = d.inputs[1];
            copy_pair copies[3]{{d.inputs[1], &s->hud}};
            size_t count = 1;

            if (f.flags & 1) {
                s->pose = f.pose;
                s->world.width = s->base.width;
                s->world.height = s->base.height;
                s->world.flip = (f.flags & 8) != 0;

                okay = okay && valid_world_dispatch(f.flags, f.world_dispatches) && f.pose.unity_frame == f.source_frame &&
                    pose_projection(f.pose, s->base.width, s->base.height, s->actual) &&
                    g->model.accept(f.pose, s->actual, s->base.width, s->base.height) &&
                    g->model.root(f.pose.root_x, f.pose.root_z, f.pose.orthographic_size, s->actual);
                s->world.source = s->base.source = s->actual;

                s->inputs[2] = d.inputs[2];
                copies[count++] = {d.inputs[2], &s->world};

                if (f.cache.serial == g->cache_packet.serial && g->cache.texture) {
                    s->cache = g->cache;
                    s->cache_dependency = g->cache_copy;
                } else {
                    s->cache.width = f.cache.width;
                    s->cache.height = f.cache.height;
                    s->cache.flip = (f.cache.flags & 2) != 0;
                    s->cache.source = {f.cache.affine[0], f.cache.affine[1], f.cache.affine[2], f.cache.affine[3], f.cache.affine[4], f.cache.affine[5]};
                    s->inputs[3] = d.inputs[3];
                    copies[count++] = {d.inputs[3], &s->cache};
                }
            } else {
                okay = okay && valid_absent_world(f);
            }

            if (!okay) {
                s->abandoned = true;
                fail(malformed);
                return;
            }

            int encoded = encode_copies(copies_api, copies, count, s->seal_copy);
            if (encoded > 0) {
                s->sealed = true;
                s->state = slot_retiring;
                s->abandoned = true;
                state.dropped_frames++;
                return;
            }

            s->sealed = true;

            if (encoded < 0) {
                if (s->seal_copy) {
                    s->state = slot_quarantined;
                } else {
                    s->sealed = false;
                    s->abandoned = true;
                }
                fail(gpu_fault);
                return;
            }

            if (s->has_world && !s->cache_dependency) s->cache_dependency = s->seal_copy;
            g->cache = s->cache;
            g->cache_copy = s->cache_dependency;
            g->cache_packet = f.cache;
            s->source_serial = ++state.source_commit;
            g->status.source_commit = s->source_serial;
            g->status.base_format = (uint32_t)s->base.texture.pixelFormat;
            g->status.world_format = s->has_world ? (uint32_t)s->world.texture.pixelFormat : 0;
            g->status.hud_format = (uint32_t)s->hud.texture.pixelFormat;
        }

        void native_marker(dispatch_ticket& d, IUnityGraphicsMetal* metal) {
            if (d.marker.session != state.session) return;

            if (d.marker.flags & 1) {
                state.flags &= ~2u;
                return;
            }

            state.native_ordered_frame = d.marker.source_frame;
            state.native_ordered_restore_serial = d.marker.restore_serial;

            if (!current_target_mode) {
                if (!target_probe_mode) arm_original_frame(d.marker);
                return;
            }

            // Tickets queued before routing restoration cannot enter a fresh
            // observation scope, even if their GPU callback arrives late.
            if (!observer_restore.allows(d.marker, state.content_fence)) return;
            int observed = present_observer::source(metal, d.target, d.marker);
            if (observed < 0) {
                observer_restore.fail(observed);
                state.dropped_frames++;
                fail(observed);
            }
        }

        void render_event(int token, void* pointer) {
            @autoreleasepool {
                if (process_exit.load()) return;

                std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
                if (!lock) {
                    if (token > 0) {
                        for (size_t i = 0; i < tickets.size(); ++i) {
                            if (&tickets[i] == pointer) {
                                missed.record(i, token);
                                break;
                            }
                        }
                    }
                    return;
                }

                if (!started) return;
                if (state.render_thread && state.render_thread != native_thread()) {
                    fail(wrong_owner);
                    return;
                }

                state.render_thread = native_thread();
                retire_sources();
                poll_original();

                dispatch_ticket* found = nullptr;
                if (token > 0) {
                    for (auto& d : tickets) {
                        if (&d == pointer && d.token == token) {
                            found = &d;
                            break;
                        }
                    }
                }

                if (!found) return;
                dispatch_ticket d = *found;
                *found = dispatch_ticket{};
                --state.queued_callbacks;

                if (d.cancelled) {
                    cancel_copy_probe(d.copy_probe_index);
                    return;
                }

                auto registry = interfaces.load(std::memory_order_acquire);
                auto graphics = registry ? registry->Get<IUnityGraphics>() : nullptr;
                auto metal = registry ? registry->Get<IUnityGraphicsMetal>() : nullptr;
                auto copies_api = registry ? registry->Get<IUnityGraphicsMetalV2>() : nullptr;

                if (!graphics || graphics->GetRenderer() != kUnityGfxRendererMetal || !metal || !copies_api) {
                    if (state.result >= 0) {
                        NSLog(@"SMF RenderEvent unavailable site=graphics-interface session=%llu token=%d kind=%u registry=%p graphics=%p metal=%p",
                            (unsigned long long)state.session, token, d.kind, (void*)registry, (void*)graphics, (void*)metal);
                    }
                    cancel_copy_probe(d.copy_probe_index);
                    fail(unavailable);
                    return;
                }

                if (target_probe_mode && d.probe.request.size) {
                    d.probe.serial = ++target_diagnostic.consumed;
                    d.probe.dispatch_token = uint64_t(token);
                    observe_target(metal, d.probe);
                    target_diagnostic.render_thread = native_thread();
                    (d.probe.request.phase == 1 ? target_diagnostic.pre_gui : target_diagnostic.end_of_frame) = d.probe;
                    if (d.copy_probe_index) execute_copy_probe(d.copy_probe_index, metal, d.probe);
                }

                if (d.kind == 1) pre_gui(d, metal, copies_api);
                else if (d.kind == 2) seal(d, copies_api);
                else native_marker(d, metal);
            }
        }

        template<class Packet>
        int queue_packet(const Packet* p, uint32_t bytes, uint32_t kind, void** ticket, int32_t* token) {
            if (!ticket || !token) return malformed;
            *ticket = nullptr;
            *token = 0;
            if (!on_main() || !p || bytes != sizeof(*p) || p->size != bytes || p->version != (kind == 2 ? 3u : 1u) || !p->source_frame) return malformed;

            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock) return 1;

            if constexpr (std::is_same_v<Packet, session_native_frame>) {
                // Valid retired markers are unavailable, never acknowledged or queued.
                int policy = native_marker_queue_policy(*p, state.session, state.operation_fence, restore_after_frame, source_released);
                if (policy) return policy;
            } else if (p->session != state.session || source_released) {
                return malformed;
            }

            pump_main();
            if (int owner_result = capture_queue_owner_result(kind, state.result)) return owner_result;

            dispatch_ticket d;
            d.kind = kind;

            if (kind != 3) {
                auto* g = find_generation(p->generation);
                if (!g || g->retiring || routing_fence || p->content_revision != state.content_fence || p->content_revision != g->status.content_revision) return malformed;
            }

            if constexpr (std::is_same_v<Packet, session_pre_gui>) {
                if (!p->width || !p->height || p->width > 16384 || p->height > 16384 || p->reserved || (p->flags & ~1u) || routing_fence) return malformed;

                // A fullscreen or display change can alter the physical window size
                // without changing Unity's logical image size or generation. Check
                // that relation during capture too; otherwise draw silently returns
                // busy forever and leaves the last overlay image on screen.
                int geometry = source_device ? window.configure(source_device, p->width, p->height) : window.refresh_geometry();
                if (geometry < 0) {
                    fail(geometry);
                    return geometry;
                }

                d.pre = *p;
                if (current_target_mode) {
                    auto* g = find_generation(p->generation);
                    if (!source_target.matches(*p)) return unavailable;
                    if (!g || p->source_frame <= g->target_queued_frame) return 1;
                    d.target = source_target.value;
                } else if (!target_probe_mode) {
                    if (!staged_base.texture || staged_base.session != p->session || staged_base.content_revision != p->content_revision ||
                        staged_base.generation != p->generation || staged_base.source_frame != p->source_frame ||
                        staged_base.width != p->width || staged_base.height != p->height) return unavailable;
                    d.base = staged_base;
                    d.inputs[0] = (__bridge id<MTLTexture>)(void*)staged_base.texture;
                }
            } else if constexpr (std::is_same_v<Packet, session_frame>) {
                if (!p->hud_texture || !p->world_texture || p->hud_texture == p->world_texture || !valid_world_dispatch(p->flags, p->world_dispatches)) return malformed;
                if (!(p->flags & 1) && !valid_absent_world(*p)) return malformed;

                d.frame = *p;
                d.inputs[1] = (__bridge id<MTLTexture>)(void*)p->hud_texture;
                d.inputs[2] = (__bridge id<MTLTexture>)(void*)p->world_texture;
                d.inputs[3] = (__bridge id<MTLTexture>)(void*)p->cache.texture;
            } else {
                d.marker = *p;

                if (current_target_mode && !(p->flags & 1)) {
                    if (!native_target.matches(*p)) return 1;
                    if (p->source_frame <= native_target_queued_frame) return 1;
                    d.target = native_target.value;
                }
            }

            // Complete native-marker validation precedes the retired-source check.
            // Consume only into the exact existing ticket. Begin-only markers never
            // steal an EOF observation, and a full callback queue keeps the stage.
            mac_target_probe_observation* target_stage = nullptr;

            if (target_probe_mode && (kind == 1 || (kind == 3 && !(d.marker.flags & 1)))) {
                auto& candidate = staged_target[kind == 1 ? 0 : 1];
                auto& r = candidate.request;
                const uint32_t width = kind == 1 ? d.pre.width : d.marker.width;
                const uint32_t height = kind == 1 ? d.pre.height : d.marker.height;
                if (r.size && r.session == p->session && r.source_frame == p->source_frame && r.phase == (kind == 1 ? 1u : 2u) && r.width == width && r.height == height) {
                    d.probe = candidate;
                    target_stage = &candidate;
                } else if (r.size && r.source_frame <= p->source_frame) {
                    candidate = {};
                    ++target_diagnostic.rejected;
                }
            }

            if (token_floor >= INT32_MAX) return unavailable;

            for (auto& item : tickets) {
                if (item.token) continue;

                d.token = int32_t(++token_floor);
                if (copy_probe_mode && d.probe.request.size) d.copy_probe_index = bind_copy_probe(d.probe.request);
                item = d;
                *ticket = &item;
                *token = d.token;
                state.queued_callbacks++;
                if (target_stage) *target_stage = {};

                if constexpr (std::is_same_v<Packet, session_pre_gui>) {
                    if (current_target_mode) {
                        find_generation(p->generation)->target_queued_frame = p->source_frame;
                        source_target.clear();
                    }
                }
                if constexpr (std::is_same_v<Packet, session_native_frame>) {
                    if (current_target_mode && !(p->flags & 1)) {
                        native_target_queued_frame = p->source_frame;
                        native_target.clear();
                    }
                }
                return 0;
            }

            return 1;
        }

    }
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* value) {
    mac::loads++;
    mac::interfaces.store(value, std::memory_order_release);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload() {
    mac::process_exit = true;
    mac::interfaces = nullptr;
}

SMF_MAC_API int64_t smf_session_clock_now() {
    return native_now();
}

SMF_MAC_API int64_t smf_session_clock_frequency() {
    return 1000000000;
}

SMF_MAC_API int smf_selection_publish(const smf_selection_state* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main()) return wrong_owner;
    return selection::state().publish(p, bytes);
}

SMF_MAC_API int smf_session_start(uint64_t original, uint64_t session) {
    using namespace mac;
    if (!pthread_main_np() || !original || !session) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    if (started && !joined) return state.session == session && (uint64_t)(__bridge void*)window.window == original ? 0 : 1;
    if (session <= session_floor || !interfaces.load() || loads.load() != 1 || process_exit.load()) return unavailable;

    int created = window.create(original);
    if (created != 0) return created;

    int input = camera_control::start(window.number);
    if (input != 0) {
        window.remove();
        return input;
    }

    for (auto& g : generations) g = {};
    for (auto& s : slots) s = source_slot{};

    state = {472, 1};
    presentations.reset();
    state.session = session;
    state.main_thread = native_thread();
    state.flags = 1;
    session_floor = session;
    started = true;
    joined = source_released = routing_fence = stop_worker = current_target_mode = target_probe_mode = copy_probe_mode = false;
    target_diagnostic = {672, 1};
    target_diagnostic.session = session;
    target_diagnostic.main_thread = native_thread();

    staged_target = {};
    worker_ready = false;
    visible = -1;
    pending = {};
    acknowledgement = {};
    staged_base = {};
    source_target.clear();
    native_target.clear();
    native_target_queued_frame = 0;

    show_transaction = show_completed_ns = generation_floor = restore_after_frame = 0;
    activation.reset();
    latest_visible.reset();
    observer_restore = {};
    source_before_restore = {};

    return 0;
}

SMF_MAC_API int smf_mac_original_base_enable(uint64_t session) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session || !started || source_released || joined) return malformed;
    return unavailable; // no silent remapping of the obsolete drawable-source ABI
}

SMF_MAC_API int smf_mac_source_target_enable(uint64_t session) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    if (session != state.session || !started || source_released || joined) return malformed;
    if (current_target_mode) return 0;
    if (target_probe_mode || generation_floor || pending.serial || has_leases() || has_dispatches() || window.visible ||
        source_device || worker.joinable() || staged_base.texture) return malformed;

    // This opt-in precedes all captures, so the old per-frame observer can
    // retire before the bounded present observer takes the exact original layer.
    int old = drawable_observer::remove();
    if (old) return old > 0 ? 1 : unavailable;
    int installed = present_observer::install(session, window.original);
    if (installed) return installed;

    current_target_mode = true;
    source_target.clear();
    native_target.clear();
    return 0;
}

SMF_MAC_API int smf_mac_source_target(const mac_source_target* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main()) return wrong_owner;
    if (!p || bytes != 64 || !valid_source_target(*p)) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (!current_target_mode || p->session != state.session) return malformed;
    if (source_released || joined || routing_fence) return 1;

    auto* g = find_generation(p->generation);
    if (!g || g->retiring || p->content_revision != state.content_fence || p->content_revision != g->status.content_revision ||
        p->width != g->status.width || p->height != g->status.height) return malformed;
    return source_target.stage(*p, std::max(g->status.last_source_frame, g->target_queued_frame));
}

SMF_MAC_API int smf_mac_native_target(const mac_source_target* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main()) return wrong_owner;
    if (!p || bytes != 64 || !valid_native_target(*p)) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (p->session != state.session) return malformed;
    if (!current_target_mode || source_released || joined || stop_worker) return 1;
    return native_target.stage(*p, native_target_queued_frame);
}

SMF_MAC_API int smf_mac_terminal_status(uint64_t session, present_observer::present_status* p, uint32_t bytes) {
    if (!p || bytes != sizeof(*p)) return -201;
    return present_observer::snapshot(session, *p);
}

SMF_MAC_API int smf_mac_terminal_source_rejection(uint64_t session, present_observer::source_rejection* p, uint32_t bytes) {
    using namespace mac;
    if (!p || bytes != sizeof(*p)) return -201;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session) return -201;

    if (source_before_restore.reason) {
        *p = source_before_restore;
        return 0;
    }

    return present_observer::first_source_rejection(session, *p);
}

SMF_MAC_API int smf_mac_source_base(const mac_source_base* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 96 || p->size != 96 || p->version != 1 || !p->texture || !p->native_render_buffer ||
        !p->source_frame || p->reserved || (p->flags & ~1u)) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (current_target_mode || target_probe_mode || p->session != state.session ||
        p->original_window != (uint64_t)(__bridge void*)window.window ||
        p->original_layer != (uint64_t)(__bridge void*)window.original || !p->width || !p->height) return malformed;

    staged_base = *p;
    pump_main();
    return 0;
}

SMF_MAC_API int smf_session_content_fence(uint64_t session, uint64_t revision) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session || !revision || revision < state.content_fence) return malformed;

    if (revision > state.content_fence) {
        source_target.clear();
        native_target.clear();
    }

    state.content_fence = revision;
    if (preparation_superseded(is_prepare(pending.operation), pending.serial, pending.content_revision, revision)) {
        // The old generation can no longer capture this content. Reject its ticket,
        // but leave source and worker leases for the explicit GPU retirement path.
        ack(0, 0, 2);
    }
    return 0;
}

SMF_MAC_API int smf_session_command(const session_command* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 88 || p->size != 88 || p->version != 1 || !p->serial || p->operation > op_stop) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    if (p->session != state.session || p->serial <= state.last_ack_serial) return malformed;
    pump_main();

    if (pending.serial) {
        if (p->serial == pending.serial) return std::memcmp(p, &pending, sizeof(pending)) ? malformed : 0;
        if (p->serial < pending.serial) return malformed;
        if (p->operation != op_restore_routing) return 1;
    }

    pending = *p;
    acknowledgement = {};

    if (state.result < 0 && (is_prepare(p->operation) || p->operation == op_activate)) {
        ack(0, state.result, 3);
        return 0;
    }

    if (is_prepare(p->operation)) {
        if (!p->generation || p->generation <= generation_floor || !p->width || !p->height || p->width > 16384 || p->height > 16384 ||
            uint64_t(p->width) * p->height * 4 > 64ull * 1024 * 1024 || (p->flags & ~1u) || p->content_revision != state.content_fence || routing_fence) {
            ack(0, malformed, 3);
            return 0;
        }

        generation_state* g = nullptr;
        for (auto& candidate : generations) {
            if (!candidate.status.generation || candidate.status.state == 5) {
                g = &candidate;
                break;
            }
        }

        if (!g) {
            pending = {};
            return 1;
        }

        *g = {};
        generation_floor = p->generation;
        g->status.generation = p->generation;
        g->status.content_revision = p->content_revision;
        g->status.width = p->width;
        g->status.height = p->height;
        g->status.flags = p->flags;
        g->status.state = 1;
    } else if (p->operation == op_activate) {
        if (target_probe_mode) {
            ack(0, unavailable, 3);
            return 0;
        }

        auto* g = find_generation(p->generation);
        activation.reset();
        smf_control_wheel_status input{};
        camera_control::wheel_status(input);

        if (!g || g->status.state != 2 || g->retiring || routing_fence) ack(0, malformed, 3);
        else if (input.state == 4) ack(0, unavailable, 3);
    } else if (p->operation == op_restore_routing) {
        routing_fence = true;
        state.operation_fence = p->serial;
        restore_after_frame = p->after_frame;
        staged_base = {};
        source_target.clear();
        native_target.clear();
        activation.reset();

        for (auto& d : tickets) {
            if (d.token) d.cancelled = true;
        }

        if (current_target_mode && (window.visible || state.active_generation)) {
            // Keep the displayed overlay until a fresh native presentation is
            // confirmed. Old callbacks keep their old faulted state and leases.
            if (!source_before_restore.reason) present_observer::first_source_rejection(state.session, source_before_restore);
            observer_restore.begin(*p);
            present_observer::stop(state.session);
            state.flags &= ~(2u | 16u);
        }

        ack((1u << 16) | (!window.visible && !state.active_generation ? 4096u : 0u));
    } else if (p->operation == op_await_native && current_target_mode && observer_restore.state() == present_observer::recovery::failed) {
        // An explicit owner retry keeps the actual routing identity, but
        // replaces the failed observation scope and its callback leases.
        session_command retry = *p;
        retry.serial = state.operation_fence;
        observer_restore.begin(retry);
        present_observer::stop(state.session);
        state.flags &= ~(2u | 16u);
    } else if (p->operation == op_invalidate) {
        state.content_acknowledged = state.content_fence;
        ack(8);
    } else if (p->operation == op_retire_generation) {
        auto* g = find_generation(p->generation);
        if (!g) {
            ack(512 | 4096);
        } else {
            g->retiring = true;
            g->status.retire_requested = p->serial;

            for (auto& s : slots) {
                if (s.generation == p->generation) s.abandoned = true;
            }

            if (!has_leases(p->generation) && !has_dispatches(p->generation)) {
                g->status.state = 5;
                ack(512 | 4096);
            }
        }
    } else if (p->operation == op_retire_session) {
        source_target.clear();
        native_target.clear();
        if (current_target_mode) present_observer::stop(state.session);
        retire_copy_probe_stages();

        for (auto& g : generations) g.retiring = true;
        for (auto& s : slots) s.abandoned = true;
        for (auto& d : tickets) {
            if (d.token) d.cancelled = true;
        }

        if (!has_leases() && !has_dispatches() && !copy_probe_in_flight() && !budget().bytes && !source_device) {
            source_released = true;
            ack(512 | 4096);
        }
    } else if (p->operation == op_stop && source_released && !has_leases() && !has_dispatches() && !window.visible) {
        stop_worker = true;
        camera_control::stop();
        if (!worker.joinable()) worker_exited = true;
    } else if (p->operation == op_release_main || p->operation == op_release_session) {
        ack(0, malformed, 3);
    }

    pump_main();
    return 0;
}

SMF_MAC_API int smf_session_ack(uint64_t session, uint64_t serial, session_ack* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 80) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    pump_main();
    if (acknowledgement.session != session || acknowledgement.serial != serial) return 1;
    *p = acknowledgement;
    return 0;
}

SMF_MAC_API int smf_session_status(session_status* p, uint32_t bytes) {
    using namespace mac;
    if (!p || bytes != 472) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (on_main()) pump_main();
    *p = state;
    for (size_t i = 0; i < 2; ++i) p->generations[i] = generations[i].status;
    return 0;
}

SMF_MAC_API int32_t smf_mac_presentation(mac_presentation* p, uint32_t bytes) {
    using namespace mac;
    if (!p || bytes != 48 || p->size != 48 || p->version != 1 || !pthread_main_np()) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (!on_main() || state.result < 0 || !state.active_generation || routing_fence || !presentations ||
        presentations->session != state.session || presentations->generation != state.active_generation) return 1;
    *p = {48, 1, state.session, state.active_generation, presentations->count.load(std::memory_order_relaxed), native_now(), 1000000000};
    return 0;
}

SMF_MAC_API int smf_mac_capabilities(mac_capabilities* p, uint32_t bytes) {
    using namespace mac;
    if (!p || bytes != 136) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    smf_control_wheel_status input{};
    camera_control::wheel_status(input);

    *p = {};
    p->size = 136;
    p->version = 1;
    p->session = state.session;
    p->original_window = (uint64_t)(__bridge void*)window.window;
    p->original_layer = (uint64_t)(__bridge void*)window.original;
    p->overlay_layer = (uint64_t)(__bridge void*)window.layer;
    p->native_loads = loads.load();
    p->interfaces = (uint64_t)interfaces.load();
    p->source_device = (uint64_t)(__bridge void*)source_device;
    p->main_thread = state.main_thread;
    p->render_thread = state.render_thread;
    p->worker_thread = state.worker_thread;
    p->source_completed = state.source_completed;
    p->worker_completed = state.worker_completed;
    p->original_presented = state.native_present_serial;
    p->worker_presented = latest_visible && latest_visible->presented.load(std::memory_order_acquire) ? latest_visible->serial : 0;
    p->original_presented_with_overlay = current_target_mode
        ? ((state.flags & 16) ? state.native_present_serial : 0)
        : drawable_observer::snapshot().presented_behind_overlay;

    uint32_t flags = 0;
    if (interfaces.load()) flags |= capability_graphics_interface;
    if (source_device) flags |= capability_source_base_ready;
    if (native_observer_available()) flags |= capability_original_present_observer;
    if (state.flags & 16) flags |= capability_original_presented_behind_overlay;
    if (input.state == 2) flags |= capability_mouse_observation;
    if (worker_ready) flags |= capability_worker_ready;
    p->flags = flags;

    p->result = uint32_t(state.result);
    return 0;
}

SMF_MAC_API int smf_mac_observer_diagnostic(mac_observer_diagnostic* p, uint32_t bytes) {
    using namespace mac;
    if (!pthread_main_np() || !p || bytes != sizeof(*p)) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    int result = drawable_observer::read_diagnostic(*p);
    if (result) return result;

    p->session = state.session;
    p->render_thread = state.render_thread;
    return 0;
}

SMF_MAC_API int smf_session_pre_gui(const session_pre_gui* p, uint32_t bytes, void** ticket, int32_t* token) {
    return mac::queue_packet(p, bytes, 1, ticket, token);
}

SMF_MAC_API int smf_session_frame(const session_frame* p, uint32_t bytes, void** ticket, int32_t* token) {
    return mac::queue_packet(p, bytes, 2, ticket, token);
}

SMF_MAC_API int smf_session_native_frame(const session_native_frame* p, uint32_t bytes, void** ticket, int32_t* token) {
    return mac::queue_packet(p, bytes, 3, ticket, token);
}

SMF_MAC_API int smf_session_cancel(void* pointer, int32_t token) {
    using namespace mac;
    if (!on_main() || !pointer || token <= 0 || uint64_t(token) > token_floor) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    for (auto& d : tickets) {
        if (&d == pointer) {
            if (d.token == token) d.cancelled = true;
            return 0;
        }
    }
    return malformed;
}

SMF_MAC_API void* smf_session_render_event() {
    return reinterpret_cast<void*>(&mac::render_event);
}

SMF_MAC_API int smf_session_poll_joined(uint64_t session) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    if (session != state.session) return malformed;
    if (joined) return 0;
    if (!stop_worker || !source_released || !worker_exited.load(std::memory_order_acquire) || !camera_control::join()) return 1;
    if (worker.joinable()) worker.join();

    if (current_target_mode) {
        int observer = present_observer::remove(session);
        if (observer) return observer;
    }

    int removed = window.remove();
    if (removed != 0) return removed;

    joined = true;
    state.worker_state = 5;
    if (pending.serial && pending.operation == op_stop) ack(2048);
    return 0;
}

SMF_MAC_API int smf_session_quit() {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    process_exit = true;
    camera_control::stop();
    return 0;
}

SMF_MAC_API int smf_mac_target_probe_enable(uint64_t session) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session || !started || source_released || joined) return malformed;
    if (target_probe_mode) return 0;
    if (current_target_mode || generation_floor || pending.serial || has_leases() || has_dispatches() || window.visible ||
        source_device || worker.joinable() || staged_base.texture) return malformed;

    target_probe_mode = true;
    target_diagnostic.flags = 3;
    return 0;
}

SMF_MAC_API int smf_mac_target_probe_stage(const mac_target_probe_request* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 48 || p->size != 48 || p->version != 1 || !p->source_frame || !p->native_render_buffer ||
        !p->width || !p->height || p->width > 16384 || p->height > 16384 || p->phase < 1 || p->phase > 2 || p->reserved) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;

    if (!target_probe_mode || p->session != state.session || source_released || joined) return malformed;
    auto& stage = staged_target[p->phase - 1];
    if (stage.request.size) {
        ++target_diagnostic.rejected;
        if (p->source_frame <= stage.request.source_frame) return 1;
    }

    stage = {};
    stage.request = *p;
    stage.stage_thread = native_thread();
    stage.stage_ns = native_now();
    ++target_diagnostic.staged;
    return 0;
}

SMF_MAC_API int smf_mac_target_probe_diagnostic(mac_target_probe_diagnostic* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != sizeof(*p)) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    *p = target_diagnostic;
    return 0;
}

SMF_MAC_API int smf_mac_copy_probe_enable(uint64_t session) {
    using namespace mac;
    if (!on_main()) return wrong_owner;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session || !started || !target_probe_mode || source_released || joined) return malformed;
    if (copy_probe_mode) return 0;
    if (generation_floor || pending.serial || has_leases() || has_dispatches() || window.visible || source_device || worker.joinable()) return malformed;

    int result = enable_copy_probe(session);
    if (!result) copy_probe_mode = true;
    return result;
}

SMF_MAC_API int smf_mac_copy_probe_stage(const mac_copy_probe_request* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 80 || p->target.phase < 1 || p->target.phase > 2) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (!copy_probe_mode || !target_probe_mode || p->target.session != state.session || source_released || joined || routing_fence) return malformed;

    // The main producer must publish the same target request first. This call
    // never creates a ticket or appends an independent render command.
    if (std::memcmp(&staged_target[p->target.phase - 1].request, &p->target, sizeof(p->target))) return malformed;
    return stage_copy_probe(*p);
}

SMF_MAC_API int smf_mac_copy_probe_record(uint64_t session, uint64_t capture_id, mac_copy_probe_record* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p || bytes != 608) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (!copy_probe_mode || session != state.session) return malformed;
    return read_copy_probe(session, capture_id, *p);
}

SMF_MAC_API int smf_mac_copy_probe_readback(uint64_t session, uint64_t capture_id, uint32_t route, void* p, uint32_t bytes) {
    using namespace mac;
    if (!on_main() || !p) return malformed;

    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (!copy_probe_mode || session != state.session) return malformed;
    return read_copy_bytes(session, capture_id, route, p, bytes);
}
