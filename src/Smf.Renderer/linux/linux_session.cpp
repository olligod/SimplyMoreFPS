#include "linux_glx.h"
#include "camera_bridge.h"
#include "camera_tuple.h"
#include "capture_diagnostic.h"
#include "scene_frame.h"
#include "scene_budget.h"
#include "clock.h"
#include "failure_diagnostic.h"
#include "glx_source_router.h"
#include "performance_status.h"
#include "platform_status.h"
#include "quit_contract.h"
#include "session_handoff.h"
#include "session_transport.h"
#include "slot_storage.h"
#include "swap_trace.h"
#include "../common/camera_packets.h"
#include "../common/selection_overlay.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

#define SMF_SESSION_API extern "C" __attribute__((visibility("default")))

namespace linux_session {
    namespace {

        constexpr int unsupported = -200;
        constexpr int malformed = -201;
        constexpr int wrong_owner = -202;
        constexpr int gpu_fault = -203;

        enum op {
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

        struct dispatch {
            uint64_t generation = 0;
            uint32_t kind = 0;
            pre_gui_packet pre{};
            frame_packet frame{};
            native_frame_packet marker{};
            smf_scene::snapshot scene{};
            bool has_scene = false;
        };

        struct slot {
            lease_policy lease{};
            slot_storage storage{};
            uint64_t generation = 0;
            uint64_t content = 0;
            uint64_t frame = 0;
            layer base{};
            layer world{};
            layer hud{};
            layer cache{};
            pose_packet pose{};
            affine captured{}; // screen affine the source frame was rendered with
            GLsync producer = nullptr;
            GLsync consumer = nullptr;
            bool complete = false;
            bool abandoned = false;
            smf_scene::snapshot scene{};
            std::array<GLuint, smf_scene::maximum_images> scene_resources{};
            bool has_scene = false;
        };

        struct generation_state {
            generation_status_packet status{};
            camera_model model{};
            cache_packet cache{};
            bool retiring = false;
        };

        struct storage_plan {
            texture_storage textures[4]{};
            texture_storage scene_textures[smf_scene::maximum_images]{};
            uint64_t bytes = 0;
        };

        struct draw_packet {
            int index = -1;
            uint64_t generation = 0;
            uint64_t content = 0;
            uint64_t frame = 0;
            layer base;
            layer world;
            layer hud;
            layer cache;
            pose_packet pose{};
            affine captured{};
            camera_model model{};
            smf_scene::snapshot scene{};
            std::array<GLuint, smf_scene::maximum_images> scene_resources{};
            bool has_scene = false;
        };

        status_packet empty_status() {
            status_packet p{};
            p.size = sizeof(p);
            p.version = 1;
            return p;
        }

        // Leaked on purpose: static destruction must never race a late Unity callback.
        std::mutex& gate = *new std::mutex();
        dispatch_pool<dispatch>& dispatches = *new dispatch_pool<dispatch>();
        source_glx& source = *new source_glx();
        worker_glx& worker = *new worker_glx();
        std::thread& worker_thread = *new std::thread();

        slot slots[6];
        generation_state generations[2];
        status_packet state = empty_status();
        command_packet pending{};
        ack_packet acknowledgement{};
        uint64_t xid = 0;
        uint64_t session_floor = 0;
        uint32_t main_thread = 0;
        int visible = -1;
        bool started = false;
        bool source_ready = false;
        bool worker_ready = false;
        bool source_released = false;
        bool stopping = false;
        bool joined = false;
        bool routing_fence = false;
        std::atomic<bool> exited{false};
        std::atomic<bool> process_exit{false};
        bool terminal_joined = false; // gate-owned; says nothing about GL resource retirement
        std::atomic<uint64_t> callback_drops{0};

        worker_presentation activation_presentation;
        uint64_t activation_serial = 0;
        frame_key activation_key{};
        glx_source_router router;
        surface_handoff handoff;
        presented last_native{};

        command_inbox inbox;
        snapshot<status_packet> status_mailbox;
        snapshot<ack_packet> ack_mailbox;
        snapshot<platform_status> platform_mailbox;
        platform_status platform;
        std::atomic<uint64_t> active_session{0};
        std::atomic<uint64_t> requested_content{0};
        capture_diagnostic first_capture_failure{};
        snapshot<capture_diagnostic> capture_failure_mailbox;
        failure_diagnostic first_failure{};
        snapshot<failure_diagnostic> failure_mailbox;
        performance_status performance;
        snapshot<performance_status> performance_mailbox;
        thread_local uint64_t wait_result = 0;
        thread_local uint64_t wait_kind = 0;
        thread_local uint64_t drain_stage = 0;

        std::atomic<bool> accepting_packets{false};
        hidden_surface active_surface;
        hidden_surface prepared_surface;
        hidden_surface retiring_surface;
        GLsync ownership_drain = nullptr;
        bool worker_bound = false;
        bool resize_pending = false;
        bool source_rebound_for_resize = false;
        uint64_t hidden_floor = 0;
        uint64_t resize_floor = 0;
        affine last_displayed_affine{};
        smf_scene::view last_displayed_scene{};
        command_packet deferred_command{};
        bool has_deferred = false;

        int apply_command(const command_packet*);
        void reject_obsolete_preparation();

        void publish_status(bool boundary = false) {
            status_packet current = state;
            current.dropped_frames += callback_drops.load();
            current.queued_callbacks = dispatches.occupied();
            for (int i = 0; i < 2; i++) current.generations[i] = generations[i].status;

            if (boundary) {
                status_mailbox.publish_boundary(current, native_now());
            } else {
                status_mailbox.try_publish(current, native_now());
            }

            if (acknowledgement.serial) {
                if (boundary) {
                    ack_mailbox.publish_boundary(acknowledgement, native_now());
                } else {
                    ack_mailbox.try_publish(acknowledgement, native_now());
                }
            }

            platform.phase = uint32_t(handoff.phase());
            platform.source_drawable = source.drawable;
            platform.original = xid;
            platform.hidden_drawable = active_surface.window;
            platform.source_context = reinterpret_cast<uintptr_t>(source.context);
            platform.source_thread = state.render_thread;
            platform.worker_thread = state.worker_thread;
            platform.publication++;
            platform.published_ns = native_now();
            platform_mailbox.try_publish(platform, platform.published_ns);

            if (first_capture_failure.stage) capture_failure_mailbox.try_publish(first_capture_failure, first_capture_failure.published_ns);
            if (first_failure.line) failure_mailbox.try_publish(first_failure, first_failure.published_ns);

            performance.session = state.session;
            performance.publication++;
            performance.published_ns = native_now();
            for (unsigned i = 0; i < 16; i++) performance.values[i] = source.performance[i];
            performance_mailbox.try_publish(performance, performance.published_ns);
        }

        void apply_inbox() {
            uint64_t content = requested_content.load(std::memory_order_acquire);
            if (content > state.content_fence) state.content_fence = content;
            reject_obsolete_preparation();

            command_packet next{};
            if (has_deferred) {
                next = deferred_command;
            } else if (!inbox.try_consume(next)) {
                return;
            }

            has_deferred = apply_command(&next) == 1;
            if (has_deferred) deferred_command = next;
        }

        struct boundary {
            ~boundary() {
                publish_status();
            }
        };

        generation_state* find_generation(uint64_t id) {
            for (auto& g : generations) {
                if (g.status.generation == id && id) return &g;
            }
            return nullptr;
        }

        bool on_main_thread() {
            return main_thread == native_thread();
        }

        bool is_prepare(uint32_t operation) {
            return operation == op_prepare || operation == op_replace;
        }

        bool any_resources(uint64_t generation = 0) {
            for (const auto& s : slots) {
                if (s.lease.state != lease_free && (!generation || s.generation == generation)) return true;
                if (s.storage.has_names() && (!generation || s.storage.generation == generation)) return true;
            }
            return false;
        }

        bool pending_dispatch(uint64_t generation = 0) {
            return generation ? dispatches.uses(generation) : !dispatches.empty();
        }

        void ack(uint32_t evidence, int result = 0, uint32_t disposition = 1, uint64_t frame = 0) {
            acknowledgement = {80, 1, pending.session, pending.serial, pending.generation, pending.content_revision, frame,
                               pending.operation, evidence, result, disposition, state.worker_completed, native_now()};
            state.last_ack_serial = pending.serial;
            pending = {};
        }

        void reject_obsolete_preparation() {
            if (preparation_superseded(is_prepare(pending.operation), pending.serial, pending.content_revision, state.content_fence)) {
                // Keep GL resources and queued dispatches until their retirement ACK.
                ack(0, 0, 2);
            }
        }

        void fail_at(int error, uint32_t line, const uint64_t* draw = nullptr) {
            if (!first_failure.line) {
                failure_diagnostic f;
                f.line = line;
                f.thread = native_thread();
                f.publication = 1;
                f.published_ns = native_now();
                f.session = state.session;
                f.phase = uint64_t(handoff.phase());
                f.error = error;

                if (f.thread == state.worker_thread) {
                    f.owner = 2;
                } else if (f.thread == state.render_thread) {
                    f.owner = 1;
                } else if (f.thread == state.main_thread) {
                    f.owner = 3;
                }

                f.flags = (source_ready ? 1u : 0u) | (worker_ready ? 2u : 0u) | (worker_bound ? 4u : 0u) | (routing_fence ? 8u : 0u) |
                          (resize_pending ? 16u : 0u) | (process_exit.load() ? 32u : 0u);
                f.status = state;
                for (int i = 0; i < 2; i++) f.status.generations[i] = generations[i].status;
                f.pending = pending;
                if (draw) std::memcpy(f.draw, draw, sizeof(f.draw));

                f.protocol[0] = reinterpret_cast<uintptr_t>(glXGetCurrentContext());
                f.protocol[1] = reinterpret_cast<uintptr_t>(glXGetCurrentDisplay());
                f.protocol[2] = glXGetCurrentDrawable();

                bool owns = (f.owner == 1 && glXGetCurrentContext() == source.context) || (f.owner == 2 && glXGetCurrentContext() == worker.context);
                if (owns && glXGetCurrentContext()) {
                    f.protocol[3] = 1;
                    f.protocol[4] = glGetError(); // may predate this operation
                }

                f.protocol[5] = wait_kind;
                f.protocol[6] = wait_result;
                f.protocol[7] = drain_stage;

                if (f.owner == 2) {
                    f.protocol[8] = activation_presentation.last_wait();
                    f.protocol[9] = uint64_t(int64_t(activation_presentation.fault()));
                    f.protocol[10] = activation_presentation.pending();
                }
                if (f.owner == 1) {
                    f.protocol[11] = router.last_wait();
                    f.protocol[12] = uint64_t(int64_t(router.fault()));
                }

                f.protocol[13] = requested_content.load();
                f.protocol[14] = callback_drops.load();
                f.protocol[15] = dispatches.occupied();
                latch_failure(first_failure, f);
            }

            if (state.result >= 0) state.result = error;
            state.stage = 6;
            state.worker_state = 4;
            routing_fence = true;
            if (pending.serial) ack(0, state.result, 3);
        }

#define FAIL(error) fail_at((error), __LINE__)
#define FAIL_DRAW(error, facts) fail_at((error), __LINE__, (facts))

        void capture_failure(uint32_t stage, int error, const frame_packet* frame = nullptr, const pre_gui_packet* pre = nullptr) {
            if (first_capture_failure.stage) return;

            capture_diagnostic detail;
            detail.stage = stage;
            detail.render_thread = native_thread();
            detail.publication = 1;
            detail.published_ns = native_now();
            detail.session = state.session;
            detail.content_fence = state.content_fence;
            detail.active_generation = state.active_generation;
            detail.source_commit = state.source_commit;
            detail.error = error;
            detail.pending = pending;

            if (frame) detail.frame = *frame;
            if (pre) detail.pre_gui = *pre;

            uint64_t generation = 0;
            if (frame) {
                generation = frame->generation;
            } else if (pre) {
                generation = pre->generation;
            }

            if (auto* g = find_generation(generation)) {
                detail.previous_cache = g->cache;
                const auto& m = g->model;
                double values[] = {m.nominal.a, m.nominal.b, m.nominal.c, m.nominal.d, m.nominal.e, m.nominal.f,
                                   m.x, m.z, m.logical_root_size, m.projection_half_height, double(m.width), double(m.height)};
                for (unsigned i = 0; i < 12; i++) detail.model[i] = values[i];
                detail.model_revision = m.revision;
                detail.model_map = m.map;
            }

            if (stage == capture_hud_copy || stage == capture_world_copy || stage == capture_cache_copy || stage == capture_pre_gui_copy) {
                detail.copy = source.last_copy;
            }

            remember_first_failure(first_capture_failure, detail);
        }

        void poll_native() {
            presented p;
            if (!router.poll_original(p)) return;

            last_native = p;
            if (!p.complete(xid)) {
                FAIL(gpu_fault);
                return;
            }

            state.flags |= 2;
            state.native_submitted_frame = p.key.frame;
            state.native_reveal_frame = p.key.frame;
            state.native_submitted_generation = p.key.generation;
            state.native_submitted_content = p.key.content;
            state.native_submitted_restore_serial = p.key.restore;
            state.native_present_serial++;
            state.native_backbuffer = xid;
        }

        void try_native_ack() {
            if (pending.serial && pending.operation == op_await_native &&
                handoff.native_available(last_native, pending.after_frame, pending.content_revision)) {
                ack(128, 0, 1, last_native.key.frame);
            }
        }

        void trim_scene_storage(slot_storage& storage, uint32_t count) {
            for (uint32_t i = count; i < smf_scene::maximum_images; ++i) {
                auto& texture = storage.scene_textures[i];
                if (texture.name) {
                    glDeleteTextures(1, &texture.name);
                    source.performance[deleted_names]++;
                    source.performance[live_names]--;
                }
                texture = {};
                storage.scene_images[i] = {};
            }
        }

        void delete_storage(slot_storage& storage) {
            for (auto& texture : storage.textures) {
                if (!texture.name) continue;
                glDeleteTextures(1, &texture.name);
                source.performance[deleted_names]++;
                source.performance[live_names]--;
            }
            trim_scene_storage(storage, 0);
            storage = {};
        }

        void reset_retired_slot(slot& s) {
            slot_storage retained = s.storage;
            s = slot{};
            s.storage = retained;
        }

        uint64_t source_storage_bytes() {
            uint64_t bytes = 0;
            for (const auto& item : slots) bytes += item.storage.allocated_bytes();
            return bytes;
        }

        bool has_scene_storage() {
            for (const auto& item : slots) if (item.storage.has_scene()) return true;
            return false;
        }

        storage_admission make_storage_room(slot& owner, uint64_t required, bool retain_visible) {
            const uint64_t visible_bytes = retain_visible && visible >= 0 && &slots[visible] != &owner &&
                slots[visible].lease.state == lease_worker_owned ? slots[visible].storage.allocated_bytes() : 0;
            auto result = scene_capacity(source_storage_bytes(), owner.storage.allocated_bytes(), required, visible_bytes);
            if (result != storage_admission::busy) return result;
            for (bool obsolete : {true, false}) {
                for (auto& item : slots) {
                    if (&item == &owner || item.lease.state != lease_free || !item.storage.has_names()) continue;
                    const bool stale = !item.storage.compatible(owner.storage.generation, owner.storage.content,
                        owner.storage.width, owner.storage.height);
                    if (stale != obsolete) continue;
                    delete_storage(item.storage);
                    result = scene_capacity(source_storage_bytes(), owner.storage.allocated_bytes(), required, visible_bytes);
                    if (result != storage_admission::busy) return result;
                }
            }
            return storage_admission::busy;
        }

        bool read_storage_plan(const slot& s, const frame_packet& f, const smf_scene::snapshot* scene, storage_plan& plan) {
            const auto describe = [&](texture_storage& target, uint32_t width, uint32_t height, uint32_t format) {
                if (!width || !height || width > 16384 || height > 16384) return false;
                target.width = width;
                target.height = height;
                target.format = format;
                target.bytes = uint64_t(width) * height * (format == GL_RGBA16F ? 8 : 4);
                plan.bytes += target.bytes;
                return true;
            };
            if (!describe(plan.textures[base_storage], s.base.width, s.base.height, GL_RGBA8) ||
                !describe(plan.textures[hud_storage], s.base.width, s.base.height, GL_RGBA8)) return false;
            if ((f.flags & 1) && !describe(plan.textures[world_storage], s.base.width, s.base.height, GL_RGBA8)) return false;
            if (!scene && f.cache.texture && !describe(plan.textures[cache_storage], f.cache.width, f.cache.height, GL_RGBA8))
                return false;
            if (!scene) return true;
            gl_state before;
            bool okay = true;
            for (uint32_t i = 0; i < scene->frame.image_count; ++i) {
                const auto& image = scene->images[i];
                const auto& retained = s.storage.scene_textures[i];
                GLint format = retained.format;
                if (!retained.name || std::memcmp(&image, &s.storage.scene_images[i], sizeof(image))) {
                    if (!glIsTexture(GLuint(image.texture))) { okay = false; break; }
                    glBindTexture(GL_TEXTURE_2D, GLuint(image.texture));
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
                }
                const bool depth = (image.flags & smf_scene::depth_image) != 0;
                if (depth ? format != GL_R32F : format != GL_RGBA16F && format != GL_RGBA8 && format != GL_SRGB8_ALPHA8) {
                    okay = false;
                    break;
                }
                if (!describe(plan.scene_textures[i], image.width, image.height,
                    format == GL_SRGB8_ALPHA8 ? GL_RGBA8 : GLenum(format))) { okay = false; break; }
            }
            return before.restore() && okay;
        }

        void retain_planned_storage(slot_storage& storage, const storage_plan& plan) {
            const auto retain = [&](texture_storage& allocated, const texture_storage& wanted) {
                if (!allocated.name || (wanted.bytes && allocated.width == wanted.width &&
                    allocated.height == wanted.height && allocated.format == wanted.format)) return;
                glDeleteTextures(1, &allocated.name);
                source.performance[deleted_names]++;
                source.performance[live_names]--;
                allocated = {};
            };
            for (uint32_t i = 0; i < 4; ++i) retain(storage.textures[i], plan.textures[i]);
            if (!storage.textures[cache_storage].name) storage.cache_valid = false;
            for (uint32_t i = 0; i < smf_scene::maximum_images; ++i) {
                retain(storage.scene_textures[i], plan.scene_textures[i]);
                if (!storage.scene_textures[i].name) storage.scene_images[i] = {};
            }
        }

        bool copy_layer(slot& s, storage_layer which, GLuint texture, layer& out, uint32_t width, uint32_t height,
                        bool original = false) {
            auto& storage = s.storage.textures[which];
            if (storage.name && (storage.width != width || storage.height != height)) {
                // The slot is back with its source owner, so no worker samples this storage now.
                glDeleteTextures(1, &storage.name);
                source.performance[deleted_names]++;
                source.performance[live_names]--;
                storage = {};
                if (which == cache_storage) s.storage.cache_valid = false;
            }

            storage.width = width;
            storage.height = height;
            storage.format = GL_RGBA8;
            storage.bytes = uint64_t(width) * height * 4;
            bool copied = source.copy(texture, storage.name, width, height, original, GL_RGBA8);
            if (copied) out.texture = storage.name;
            return copied;
        }

        bool copy_scene(slot& s, const smf_scene::snapshot& scene, const storage_plan& plan) {
            s.has_scene = false;
            trim_scene_storage(s.storage, scene.frame.image_count);
            for (auto& other : slots) {
                if (other.lease.state == lease_free && other.storage.compatible(s.generation, s.content, s.base.width, s.base.height))
                    trim_scene_storage(other.storage, scene.frame.image_count);
            }
            for (uint32_t i = 0; i < scene.frame.image_count; ++i) {
                const auto& image = scene.images[i];
                auto& storage = s.storage.scene_textures[i];
                auto& previous = s.storage.scene_images[i];
                if (!storage.name || std::memcmp(&image, &previous, sizeof(image))) {
                    const GLenum owned_format = plan.scene_textures[i].format;
                    if (storage.name && (storage.width != image.width || storage.height != image.height || storage.format != owned_format)) {
                        glDeleteTextures(1, &storage.name);
                        source.performance[deleted_names]++;
                        source.performance[live_names]--;
                        storage = {};
                    }
                    previous = {};
                    storage.width = image.width;
                    storage.height = image.height;
                    storage.format = owned_format;
                    storage.bytes = plan.scene_textures[i].bytes;
                    if (!source.copy(GLuint(image.texture), storage.name, image.width, image.height, false, owned_format)) return false;
                    previous = image;
                }
                s.scene_resources[i] = storage.name;
            }
            s.scene = scene;
            s.has_scene = true;
            return true;
        }

        bool signaled(GLsync sync) {
            if (!sync) return false;

            GLenum r = glClientWaitSync(sync, 0, 0);
            wait_kind = 1;
            wait_result = r;
            if (r == GL_WAIT_FAILED) FAIL(gpu_fault);
            return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
        }

        void source_retire() {
            for (auto& s : slots) {
                if (s.lease.state == lease_returning && signaled(s.consumer)) {
                    glDeleteSync(s.consumer);
                    s.consumer = nullptr;
                    s.lease.reclaim(true);
                    reset_retired_slot(s);
                } else if (s.lease.state == lease_source_writing || s.lease.state == lease_published) {
                    auto* g = find_generation(s.generation);
                    bool unwanted = s.abandoned || routing_fence || state.result < 0 || !g || g->retiring;
                    if (unwanted && signaled(s.producer) && s.lease.retire_unacquired(true)) {
                        glDeleteSync(s.producer);
                        s.producer = nullptr;
                        reset_retired_slot(s);
                    }
                }
            }

            // Purge before any retirement ack; a free slot can still own names.
            if (source_ready && source.own()) {
                for (auto& s : slots) {
                    auto* g = find_generation(s.storage.generation);
                    if (storage_needs_purge(s.storage, s.lease, g != nullptr, g && g->retiring, routing_fence || state.result < 0 || source_released)) {
                        delete_storage(s.storage);
                    }
                }
            }

            // A retire is acknowledged only once every callback, copy, worker read, fence and delete is done.
            for (auto& g : generations) {
                if (g.retiring && !any_resources(g.status.generation) && !pending_dispatch(g.status.generation)) {
                    g.status.state = 5;
                    g.status.retired_serial = g.status.retire_requested;
                }
            }

            if (pending.serial && pending.operation == op_retire_generation) {
                auto* g = find_generation(pending.generation);
                if ((!g || g->status.state == 5) && !any_resources(pending.generation) && !pending_dispatch(pending.generation)) ack(512, 0, 1);
            }

            if (pending.serial && pending.operation == op_retire_session && !any_resources() && !pending_dispatch()) {
                if (!router.marker_pending() && router.restore_hooks()) {
                    source.release();
                    source_released = true;
                    accepting_packets = false;
                    ack(512, 0, 1);
                }
            }
        }

        void hand_back(slot& s) {
            if (s.lease.state != lease_worker_owned) return;

            s.consumer = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!s.consumer) {
                s.lease.state = lease_quarantined;
                FAIL(gpu_fault);
                return;
            }

            glFlush();
            s.lease.hand_back(true);
        }

        int best_slot(uint64_t generation) {
            int best = -1;
            for (int i = 0; i < 6; i++) {
                const auto& s = slots[i];
                if (s.lease.state != lease_worker_owned || !s.complete || s.abandoned || s.generation != generation) continue;
                if (s.frame <= hidden_floor || s.frame <= resize_floor) continue;
                if (best < 0 || s.frame > slots[best].frame) best = i;
            }
            return best;
        }

        void drain_worker_slots() {
            for (int i = 0; i < 6; i++) {
                auto& s = slots[i];
                if (i == visible || s.lease.state != lease_worker_owned) continue;
                auto* g = find_generation(s.generation);
                const bool warmup = pending.serial && pending.operation == op_prepare &&
                    s.generation == pending.generation && s.content == pending.content_revision &&
                    s.content == state.content_fence && s.frame == last_native.key.frame;
                if (!g || g->retiring || s.abandoned || s.content != state.content_fence ||
                    (!warmup && i != best_slot(s.generation))) hand_back(s);
            }
        }

        bool pin(int index, draw_packet& out) {
            if (index < 0 || slots[index].lease.state != lease_worker_owned) return false;

            auto& s = slots[index];
            auto* g = find_generation(s.generation);
            if (!g) return false;

            out = {index, s.generation, s.content, s.frame, s.base, s.world, s.hud, s.cache, s.pose, s.captured, g->model};
            out.scene = s.scene;
            out.scene_resources = s.scene_resources;
            out.has_scene = s.has_scene;
            return true;
        }

        // Draw facts are a fixed layout the C# diagnostics read; keep every index.
        void draw_identity(const draw_packet& s, bool camera, bool frozen, uint64_t* d) {
            d[0] = 1;
            d[1] = 1;
            d[2] = s.generation;
            d[3] = s.content;
            d[4] = s.frame;
            d[5] = s.pose.camera_epoch;
            d[6] = uint64_t(int64_t(s.pose.map_id));
            d[7] = s.base.width;
            d[8] = s.base.height;
            d[9] = s.world.texture;
            d[10] = camera;
            d[11] = frozen;
            d[43] = uint64_t(int64_t(s.model.map));
            d[44] = s.model.revision;
            d[45] = uint64_t(int64_t(s.index));
            d[58] = state.content_fence;
            d[60] = native_now();
        }

        void record_geometry_facts(uint64_t* facts, const uint64_t* geometry_facts, geometry_outcome geometry) {
            facts[21] = geometry_facts[1];
            facts[22] = geometry_facts[2];
            facts[23] = geometry_facts[3];
            facts[24] = geometry_facts[4];
            facts[27] = uint64_t(geometry);
            facts[28] = geometry_facts[5];
            facts[29] = geometry_facts[6];
        }

        draw_outcome render_pinned(const draw_packet& s, geometry_ticket ticket, bool camera, bool frozen,
                                  affine& desired, smf_scene::view& scene_view, uint64_t* d) {
            d[10] = camera;
            d[11] = frozen;
            desired = frozen ? last_displayed_affine : s.captured;
            double scene_x = s.pose.x;
            double scene_z = s.pose.z;
            const auto overlay_snapshot = worker.overlay.latch();

            double x = 0;
            double y = 0;
            bool middle = false;
            bool focus = false;
            bool left = false;
            bool pointer = false;

            if (s.world.texture && !frozen) {
                pointer = camera_control::observe(worker.input_display, worker.original, x, y, middle, focus, left);
                smf_bridge_desired target{};

                if (camera) {
                    d[12] = 1;
                    int result = camera_bridge::worker_prepare(focus, pointer, x, y, middle, target);
                    d[13] = uint64_t(int64_t(result));
                    d[14] = target.epoch;
                    d[15] = target.sequence;
                    d[16] = uint64_t(int64_t(target.map_id));
                    d[17] = target.flags;
                    d[18] = failure_bits(target.x);
                    d[19] = failure_bits(target.z);
                    d[20] = failure_bits(target.root_size);

                    if (result == 0) {
                        if (stale_camera_tuple(camera, s.pose.camera_epoch, s.pose.map_id, target.epoch, target.map_id)) {
                            d[1] = target.epoch != s.pose.camera_epoch ? 2 : 3;
                            return draw_outcome::stale_camera;
                        }
                        const bool projected = s.has_scene
                            ? scene_projection(s.pose, s.model, s.captured, target.x, target.z,
                                target.projection_half_height, desired, scene_x, scene_z)
                            : s.model.root(target.x, target.z, target.projection_half_height, desired);
                        if (!projected) {
                            d[1] = 4;
                            return draw_outcome::failed;
                        }
                    }
                }
            }

            const double values[] = {desired.a, desired.b, desired.c, desired.d, desired.e, desired.f,
                                     s.captured.a, s.captured.b, s.captured.c, s.captured.d, s.captured.e, s.captured.f};
            for (unsigned i = 0; i < 12; i++) d[31 + i] = failure_bits(values[i]);

            auto overlay_geometry = worker.overlay.read(overlay_snapshot, active_session.load(), s.content, s.pose.map_id, desired,
                                                        focus, pointer, left, x, y);
            if (frozen || !s.world.texture) overlay_geometry.visible = false;

            d[1] = 5;
            smf_scene::frame scene_frame{&s.scene, s.scene_resources.data()};
            scene_view = {};
            scene_view.width = frozen ? s.base.width : ticket.width;
            scene_view.height = frozen ? s.base.height : ticket.height;
            const double scene_affine[] = {desired.a, desired.b, desired.c, desired.d, desired.e, desired.f};
            std::copy(std::begin(scene_affine), std::end(scene_affine), scene_view.map_affine);
            scene_view.camera_x = scene_x;
            scene_view.camera_z = scene_z;
            if (frozen && s.has_scene) scene_view = last_displayed_scene;
            draw_outcome result = worker.draw(ticket, s.base, s.world, s.hud, s.cache, desired,
                                              frozen ? s.base.width : 0, frozen ? s.base.height : 0, overlay_geometry,
                                              s.has_scene ? &scene_frame : nullptr, s.has_scene ? &scene_view : nullptr);
            for (unsigned i = 0; i < 8; i++) d[50 + i] = worker.draw_facts[i];
            if (result == draw_outcome::drawn) d[1] = 6;
            return result;
        }

        // Worker-only GL work outside the gate. Reports facts only; the session ledger is
        // never touched while a source callback can own the gate.
        int drain_worker_original() {
            drain_stage = 1;
            if (!worker_bound) return 1;

            drain_stage = 2;
            if (!ownership_drain) {
                ownership_drain = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush();
                if (!ownership_drain) return -1;
            }

            drain_stage = 3;
            GLenum result = glClientWaitSync(ownership_drain, 0, 0);
            wait_kind = 2;
            wait_result = result;
            if (result == GL_WAIT_FAILED) return -1;
            if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) return 0;

            glDeleteSync(ownership_drain);
            ownership_drain = nullptr;
            XSync(worker.display, False);
            drain_stage = 4;
            if (!worker.healthy()) return -1;

            drain_stage = 5;
            if (!glXMakeContextCurrent(worker.display, None, None, nullptr)) return -1;
            drain_stage = 6;
            return 1;
        }

        void worker_loop() {
            std::unique_lock<std::mutex> lock(gate);
            state.worker_thread = native_thread();
            state.worker_state = 1;
            auto* first = find_generation(pending.generation);

            if (!first) {
                FAIL(malformed);
                exited = true;
                publish_status();
                return;
            }

            source_glx source_copy = source;
            uint32_t width = first->status.width;
            uint32_t height = first->status.height;

            lock.unlock();

            bool created = !process_exit.load() && worker.create(source_copy, xid, width, height);
            if (!created) {
                camera_control::clear();
                bool cleaned = worker.destroy();

                lock.lock();
                // Uncertain GL teardown still has an owner; keep it out of the no-worker shortcut.
                if (!cleaned) worker_ready = true;
                FAIL(unsupported);
                exited = true;
                publish_status();
                return;
            }

            lock.lock();
            active_surface = worker.take_initial_hidden();
            router.set_prepared_hidden(active_surface.window);
            worker_bound = true;
            worker_ready = true;
            state.worker_state = 2;
            handoff.worker_staged();

            platform.worker_context = reinterpret_cast<uintptr_t>(worker.context);
            std::snprintf(platform.worker_renderer, 128, "%s", worker.renderer.c_str());
            std::snprintf(platform.worker_vendor, 64, "%s", worker.vendor.c_str());
            std::snprintf(platform.worker_version, 128, "%s", worker.version.c_str());

            camera_bridge::worker_ready(native_thread(), 1000000000);
            publish_status();

            lock.unlock();

            for (;;) {
                lock.lock();
                apply_inbox();

                if (process_exit.load()) {
                    // Exit never switches source routing: Unity may still issue teardown GLX calls.
                    lock.unlock();
                    bool unbound = !worker_bound;
                    if (worker_bound && glXGetCurrentDisplay() == worker.display && glXGetCurrentContext() == worker.context) {
                        unbound = glXMakeContextCurrent(worker.display, None, None, nullptr) && glXGetCurrentContext() == nullptr;
                    }

                    camera_bridge::worker_removed();
                    camera_control::clear();
                    worker.close_input();

                    lock.lock();
                    if (unbound) worker_bound = false;
                    worker_ready = false;
                    if (!unbound) {
                        FAIL(gpu_fault);
                    } else {
                        state.worker_state = 3;
                    }

                    exited = true;
                    publish_status();
                    return;
                }
                if (inbox.activation.cancelled()) routing_fence = true;
                state.content_acknowledged = state.content_fence;
                try_native_ack();

                if (worker_bound) {
                    for (auto& s : slots) {
                        if (s.lease.state != lease_published || !signaled(s.producer)) continue;
                        glDeleteSync(s.producer);
                        s.producer = nullptr;
                        s.lease.acquire(true);
                        state.source_completed++;
                        if (auto* g = find_generation(s.generation)) {
                            g->status.completed_commit = state.source_completed;
                            g->status.last_source_frame = s.frame;
                            g->status.frames++;
                        }
                    }
                }

                if (worker_bound && activation_presentation.pending()) {
                    lock.unlock();
                    presented shown;
                    bool complete = activation_presentation.poll(worker, shown);

                    lock.lock();
                    if (activation_presentation.fault()) FAIL(gpu_fault);

                    if (complete) {
                        state.worker_completed = state.worker_commit;
                        if (pending.serial == activation_serial && pending.operation == op_activate && !routing_fence &&
                            handoff.activation_ready(shown, activation_key) && inbox.activation.commit(activation_serial)) {
                            state.active_generation = pending.generation;
                            state.active_frame = shown.key.frame;
                            if (auto* g = find_generation(pending.generation)) g->status.state = 3;
                            ack(4, 0, 1, shown.key.frame);
                        }

                        activation_serial = 0;
                    }
                }

                if (pending.serial && is_prepare(pending.operation)) {
                    auto* g = find_generation(pending.generation);
                    int index = -1;

                    if (pending.operation == op_prepare) {
                        for (int i = 0; i < 6; i++) {
                            const auto& s = slots[i];
                            if (s.lease.state == lease_worker_owned && s.complete && !s.abandoned && s.generation == pending.generation &&
                                s.frame == last_native.key.frame) {
                                index = i;
                            }
                        }
                    } else {
                        index = best_slot(pending.generation);
                    }

                    bool size_ready = g && active_surface.width == g->status.width && active_surface.height == g->status.height && !resize_pending;
                    bool native = false;
                    if (index >= 0) {
                        frame_key prepared_key{state.session, pending.generation, pending.content_revision, slots[index].frame, 0};
                        native = pending.operation == op_replace || handoff.warmup_ready(last_native, prepared_key);
                    }

                    if (g && index >= 0 && native && size_ready) {
                        g->status.state = 2;
                        g->status.prepared_frame = slots[index].frame;
                        ack(pending.operation == op_prepare ? 3 : 2, 0, 1, slots[index].frame);
                    }
                }

                // A size replacement freezes the last complete frame on the original window;
                // the source stays hidden and plain content replacements never rebind.
                if (pending.serial && is_prepare(pending.operation) && router.routing_hidden()) {
                    auto* g = find_generation(pending.generation);
                    if (g && (active_surface.width != g->status.width || active_surface.height != g->status.height) &&
                        !prepared_surface.window && !retiring_surface.window) {
                        uint32_t w = g->status.width;
                        uint32_t h = g->status.height;
                        resize_pending = true;

                        lock.unlock();
                        hidden_surface prepared;
                        geometry_outcome created_hidden = worker.create_hidden(w, h, prepared);

                        lock.lock();
                        prepared_surface = prepared;
                        if (created_hidden == geometry_outcome::failed) FAIL(unsupported);
                    }
                }

                if (retiring_surface.window && worker_bound && retiring_surface.source_fence && signaled(retiring_surface.source_fence)) {
                    glDeleteSync(retiring_surface.source_fence);
                    retiring_surface.source_fence = nullptr;
                    retiring_surface.source_departed = true;
                }

                if (retiring_surface.window && worker_bound && retiring_surface.source_departed && !retiring_surface.source_fence) {
                    hidden_surface retiring = retiring_surface;
                    lock.unlock();
                    bool removed = worker.destroy_hidden(retiring);
                    lock.lock();
                    retiring_surface = retiring;
                    if (!removed) FAIL(gpu_fault);
                }

                if (resize_pending && source_rebound_for_resize && !retiring_surface.window) {
                    resize_pending = false;
                    source_rebound_for_resize = false;
                }

                if (pending.serial && pending.operation == op_activate && !routing_fence && !activation_presentation.pending()) {
                    if (handoff.phase() == handoff_phase::worker_staging) {
                        for (auto& s : slots) hand_back(s);

                        lock.unlock();
                        int released = drain_worker_original();
                        lock.lock();

                        if (released > 0) {
                            worker_bound = false;
                            handoff.worker_released_hidden(true, true, true);
                        } else if (released < 0) {
                            FAIL(gpu_fault);
                        }
                    }

                    if (handoff.phase() == handoff_phase::source_hidden) {
                        lock.unlock();
                        bool bound = glXMakeContextCurrent(worker.display, xid, xid, worker.context);
                        lock.lock();

                        if (bound) {
                            worker.drawable = xid;
                            worker_bound = true;
                            handoff.worker_bound_original(true);
                        } else {
                            FAIL(unsupported);
                        }
                    }

                    if (handoff.phase() == handoff_phase::worker_original) {
                        int index = best_slot(pending.generation);
                        draw_packet packet;
                        if (pin(index, packet)) {
                            uint64_t serial = pending.serial;
                            frame_key key{state.session, packet.generation, packet.content, packet.frame, 0};
                            uint64_t facts[64]{};
                            draw_identity(packet, false, false, facts);
                            geometry_ticket ticket{packet.base.width, packet.base.height};
                            lock.unlock();

                            uint64_t geometry_facts[8]{};
                            geometry_outcome geometry = worker.geometry(ticket, geometry_facts);
                            record_geometry_facts(facts, geometry_facts, geometry);

                            if (geometry == geometry_outcome::ready) {
                                worker.width = ticket.width;
                                worker.height = ticket.height;
                            }

                            facts[25] = worker.width;
                            facts[26] = worker.height;
                            affine desired;
                            smf_scene::view scene_view{};
                            draw_outcome outcome = draw_outcome::failed;

                            if (geometry == geometry_outcome::ready) {
                                outcome = render_pinned(packet, ticket, false, false, desired, scene_view, facts);
                            } else if (geometry == geometry_outcome::stale) {
                                outcome = draw_outcome::stale_geometry;
                            }

                            bool drawn = outcome == draw_outcome::drawn;
                            bool submitted = drawn && !inbox.activation.cancelled() && activation_presentation.swap_and_arm(worker, key);
                            if (submitted && packet.has_scene && !worker.scene_submitted()) submitted = false;
                            if (drawn && !submitted) facts[1] = 7;

                            lock.lock();
                            facts[59] = state.content_fence;
                            facts[61] = native_now();

                            if (submitted) {
                                state.worker_commit++;
                                activation_serial = serial;
                                activation_key = key;
                                if (visible >= 0 && visible != index) hand_back(slots[visible]);
                                visible = index;
                                last_displayed_affine = desired;
                                if (packet.has_scene) last_displayed_scene = scene_view;
                            } else if (failed_activation_submission(outcome, submitted, inbox.activation.cancelled())) {
                                FAIL_DRAW(gpu_fault, facts);
                            }
                        }
                    }
                }

                if (state.active_generation && !routing_fence && !activation_presentation.pending() && handoff.phase() == handoff_phase::worker_original) {
                    auto* active = find_generation(state.active_generation);
                    bool frozen = resize_pending || (active && active->status.content_revision != state.content_fence);
                    int latest = frozen ? visible : best_slot(state.active_generation);
                    draw_packet packet;
                    if (pin(latest, packet)) {
                        bool camera_allowed = state.content_fence == packet.content;
                        uint64_t facts[64]{};
                        draw_identity(packet, camera_allowed, frozen, facts);
                        lock.unlock();

                        uint64_t geometry_facts[8]{};
                        geometry_ticket ticket{};
                        geometry_outcome geometry = worker.geometry({}, geometry_facts);
                        record_geometry_facts(facts, geometry_facts, geometry);

                        if (geometry == geometry_outcome::ready) {
                            ticket = {uint32_t(geometry_facts[3]), uint32_t(geometry_facts[4])};
                            worker.width = ticket.width;
                            worker.height = ticket.height;
                            frozen = frozen || ticket.width != packet.base.width || ticket.height != packet.base.height;
                        }

                        facts[25] = worker.width;
                        facts[26] = worker.height;
                        affine desired;
                        smf_scene::view scene_view{};
                        draw_outcome outcome = draw_outcome::failed;

                        if (geometry == geometry_outcome::ready) {
                            outcome = render_pinned(packet, ticket, !frozen && camera_allowed, frozen, desired, scene_view, facts);
                        } else if (geometry == geometry_outcome::stale) {
                            outcome = draw_outcome::stale_geometry;
                        }

                        bool drawn = outcome == draw_outcome::drawn;
                        bool swapped = drawn && !inbox.activation.cancelled();
                        if (swapped) {
                            glXSwapBuffers(worker.display, xid);
                            if (packet.has_scene && !worker.scene_submitted()) {
                                swapped = false;
                                outcome = draw_outcome::failed;
                            }
                            camera_bridge::worker_committed(swapped ? 0 : gpu_fault);
                        } else if (outcome == draw_outcome::stale_camera || outcome == draw_outcome::stale_geometry ||
                            outcome == draw_outcome::deferred) {
                            // The camera or window moved on after pinning: keep the shown frame and
                            // do not acknowledge a pose that was never drawn.
                            camera_bridge::worker_committed(S_FALSE);
                        } else if (drawn) {
                            camera_bridge::worker_reused_pose();
                        }

                        lock.lock();
                        facts[59] = state.content_fence;
                        facts[61] = native_now();

                        if (swapped) {
                            state.worker_commit++;
                            state.active_frame = packet.frame;
                            if (visible >= 0 && visible != latest) hand_back(slots[visible]);
                            visible = latest;
                            last_displayed_affine = desired;
                            if (packet.has_scene) last_displayed_scene = scene_view;
                        } else if (outcome == draw_outcome::failed) {
                            FAIL_DRAW(gpu_fault, facts);
                        }
                    }
                }

                if (worker_bound) drain_worker_slots();

                if (pending.serial && pending.operation == op_restore_routing && !activation_presentation.pending() &&
                    handoff.phase() < handoff_phase::worker_released) {
                    if (worker_bound) {
                        for (auto& s : slots) hand_back(s);
                    }

                    lock.unlock();
                    int released = drain_worker_original();
                    lock.lock();

                    if (released > 0) {
                        worker_bound = false;
                        handoff.worker_released_original(true, true, true, true);
                        visible = -1;
                    } else if (released < 0) {
                        FAIL(gpu_fault);
                    }
                }

                if (pending.serial && pending.operation == op_detach && !activation_presentation.pending() &&
                    handoff.phase() >= handoff_phase::native_fresh) {
                    state.active_generation = 0;
                    state.active_frame = 0;
                    ack(256);
                }

                if (handoff.phase() >= handoff_phase::source_restored && !worker_bound) {
                    lock.unlock();
                    bool bound = glXMakeContextCurrent(worker.display, active_surface.drawable, active_surface.drawable, worker.context);
                    lock.lock();

                    if (bound) {
                        worker.drawable = active_surface.drawable;
                        worker_bound = true;
                    } else {
                        FAIL(gpu_fault);
                    }
                }

                if (pending.serial && pending.operation == op_stop && source_released && !any_resources() && !pending_dispatch() &&
                    !has_deferred && inbox.empty()) {
                    stopping = true;
                }

                if (stopping && !retiring_surface.window) {
                    lock.unlock();
                    int retired = worker.retire_objects();
                    lock.lock();
                    if (retired < 0) FAIL(gpu_fault); // keep the owner and its uncertain resources until quit

                    if (retired > 0) {
                        hidden_surface active = active_surface;
                        hidden_surface prepared = prepared_surface;
                        active.source_departed = true;
                        prepared.source_departed = true;

                        lock.unlock();
                        camera_bridge::worker_removed();
                        bool unbound = !worker_bound || glXMakeContextCurrent(worker.display, None, None, nullptr);
                        bool cleaned = unbound && worker.destroy_hidden(prepared) && worker.destroy_hidden(active);

                        if (cleaned) {
                            worker.drawable = 0;
                            cleaned = worker.destroy();
                        }

                        lock.lock();
                        if (unbound) worker_bound = false;
                        active_surface = active;
                        prepared_surface = prepared;

                        if (!cleaned) {
                            // Keep the uncertain handles; destroyed names are never retried.
                            FAIL(gpu_fault);
                            state.worker_state = 4;
                            exited = true;
                            publish_status();
                            return;
                        }

                        worker_ready = false;
                        platform.worker_context = 0;
                        state.worker_state = 3;
                        exited = true;
                        publish_status();
                        return;
                    }
                }

                publish_status();
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        bool match(const frame_packet& f, const slot& s) {
            return f.session == state.session && f.generation == s.generation && f.content_revision == s.content && f.source_frame == s.frame;
        }

        void pre_gui(const pre_gui_packet& p) {
            if (state.result < 0 || routing_fence || source_released) return;
            auto* g = find_generation(p.generation);
            if (!g || g->retiring || p.session != state.session || p.content_revision != g->status.content_revision ||
                p.content_revision != state.content_fence) {
                return;
            }

            if (!source_ready) {
                if (!source.discover(p.width, p.height) || source.drawable != xid) {
                    source.release();
                    FAIL(unsupported);
                    return;
                }

                state.render_thread = native_thread();
                int error = router.install(source);
                if (error) {
                    // A failed rollback can leave live hooks; keep the source owner reachable for recovery.
                    if (router.installed()) {
                        source_ready = true;
                    } else {
                        source.release();
                    }
                    FAIL(error);
                    return;
                }

                source_ready = true;
                std::snprintf(platform.source_renderer, 128, "%s", source.renderer.c_str());

                // The hooks are in place before the worker can swap anything.
                worker_thread = std::thread(worker_loop);
            }

            if (!source.own() || p.width != g->status.width || p.height != g->status.height) {
                FAIL(unsupported);
                return;
            }

            // New-size callbacks drain until the end-of-frame replacement bind.
            if (router.routing_hidden() && (active_surface.width != p.width || active_surface.height != p.height)) return;

            for (auto& old : slots) {
                if (old.generation == p.generation && old.lease.state == lease_source_writing) old.abandoned = true;
            }

            source_retire();

            slot* s = nullptr;
            for (auto& candidate : slots) {
                if (candidate.storage.reusable(candidate.lease, p.generation, p.content_revision, p.width, p.height)) {
                    s = &candidate;
                    break;
                }
            }

            if (!s) {
                for (auto& candidate : slots) {
                    if (candidate.lease.state == lease_free) {
                        s = &candidate;
                        break;
                    }
                }
            }

            if (!s) {
                state.dropped_frames++;
                source.performance[no_free_slot]++;
                return;
            }

            if (s->storage.has_names()) {
                if (!s->storage.compatible(p.generation, p.content_revision, p.width, p.height)) {
                    delete_storage(s->storage);
                    source.performance[incompatible_purges]++;
                } else {
                    source.performance[reused_slots]++;
                }
            }

            s->storage.generation = p.generation;
            s->storage.content = p.content_revision;
            s->storage.width = p.width;
            s->storage.height = p.height;
            if ((p.flags & pre_gui_scene) || has_scene_storage()) {
                const uint64_t base_bytes = uint64_t(p.width) * p.height * 4;
                const auto& retained = s->storage.textures[base_storage];
                const uint64_t required = s->storage.allocated_bytes() - (retained.name ? retained.bytes : 0) + base_bytes;
                auto admission = make_storage_room(*s, required, false);
                if (admission != storage_admission::ready && s->storage.has_names()) {
                    delete_storage(s->storage);
                    s->storage.generation = p.generation;
                    s->storage.content = p.content_revision;
                    s->storage.width = p.width;
                    s->storage.height = p.height;
                    admission = make_storage_room(*s, base_bytes, false);
                }
                if (admission != storage_admission::ready) {
                    if (admission == storage_admission::unsupported) {
                        capture_failure(capture_scene_capacity, unsupported, nullptr, &p);
                        FAIL(unsupported);
                    } else {
                        state.dropped_frames++;
                    }
                    return;
                }
            }
            s->lease.begin();
            s->generation = p.generation;
            s->content = p.content_revision;
            s->frame = p.source_frame;
            s->base.width = p.width;
            s->base.height = p.height;

            bool copied = copy_layer(*s, base_storage, 0, s->base, p.width, p.height, true);
            s->producer = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();

            if (!s->producer) {
                s->storage.cache_valid = false;
                s->lease.state = lease_quarantined;
                capture_failure(capture_pre_gui_fence, gpu_fault, nullptr, &p);
                FAIL(gpu_fault);
                return;
            }

            if (!copied) {
                s->storage.cache_valid = false;
                s->abandoned = true;
                capture_failure(capture_pre_gui_copy, gpu_fault, nullptr, &p);
                FAIL(gpu_fault);
            }
        }

        void finish_frame(const frame_packet& f, const smf_scene::snapshot* scene) {
            if (state.result < 0 || routing_fence || !source_ready || source_released) return;
            auto* g = find_generation(f.generation);
            if (!g || g->retiring || f.content_revision != state.content_fence || !source.own()) return;

            slot* s = nullptr;
            for (auto& candidate : slots) {
                if (candidate.lease.state == lease_source_writing && !candidate.abandoned && match(f, candidate)) {
                    s = &candidate;
                    break;
                }
            }

            if (!s) {
                state.dropped_frames++;
                return;
            }

            auto check = [&](bool value, uint32_t stage) {
                if (!value) capture_failure(stage, malformed, &f);
                return value;
            };

            storage_plan plan{};
            if (scene || has_scene_storage()) {
                if (!read_storage_plan(*s, f, scene, plan)) {
                    s->abandoned = true;
                    capture_failure(capture_scene_capacity, malformed, &f);
                    FAIL(malformed);
                    return;
                }
                const auto admission = make_storage_room(*s, plan.bytes, true);
                if (admission != storage_admission::ready) {
                    // Keep the pre-GUI fence: this partial slot must still return to its source owner.
                    s->abandoned = true;
                    if (admission == storage_admission::unsupported) {
                        capture_failure(capture_scene_capacity, unsupported, &f);
                        FAIL(unsupported);
                    } else {
                        state.dropped_frames++;
                    }
                    return;
                }
                retain_planned_storage(s->storage, plan);
            }

            // The frame fence issued below replaces the pre-GUI fence on this same context
            // and covers every source texture read in the bundle.
            glDeleteSync(s->producer);
            s->producer = nullptr;

            cache_packet empty_cache{};
            if (!scene) {
                trim_scene_storage(s->storage, 0);
                for (auto& other : slots) {
                    if (other.lease.state == lease_free) trim_scene_storage(other.storage, 0);
                }
            }
            bool okay = check(scene ? !std::memcmp(&f.cache, &empty_cache, sizeof(empty_cache)) : valid_cache(f, g->cache), capture_cache_descriptor);
            s->hud.width = s->base.width;
            s->hud.height = s->base.height;
            s->hud.flip = (f.flags & 16) != 0;
            okay = okay && check(copy_layer(*s, hud_storage, uint32_t(f.hud_texture), s->hud, s->hud.width, s->hud.height), capture_hud_copy);

            if (f.flags & 1) {
                s->pose = f.pose;
                s->world.width = s->base.width;
                s->world.height = s->base.height;
                s->world.flip = (f.flags & 8) != 0;

                okay = okay && check(valid_world_dispatch(f), capture_world_dispatch);
                okay = okay && check(projection(f.pose, s->base.width, s->base.height, s->captured), capture_world_projection);
                okay = okay && check(g->model.accept(f.pose, s->captured, s->base.width, s->base.height), capture_world_model);
                if (!scene)
                    okay = okay && check(g->model.root(f.pose.root_x, f.pose.root_z, f.pose.orthographic_size, s->captured), capture_world_root);

                s->world.source = s->captured;
                s->base.source = s->captured;
                if (okay) {
                    okay = check(copy_layer(*s, world_storage, uint32_t(f.world_texture), s->world, s->world.width, s->world.height), capture_world_copy);
                }

                if (scene && okay) okay = check(copy_scene(*s, *scene, plan), capture_cache_copy);

                if (f.cache.texture) {
                    s->cache.width = f.cache.width;
                    s->cache.height = f.cache.height;
                    s->cache.flip = (f.cache.flags & 2) != 0;
                    s->cache.source = {f.cache.affine[0], f.cache.affine[1], f.cache.affine[2], f.cache.affine[3], f.cache.affine[4], f.cache.affine[5]};
                    okay = okay && check(f.cache.serial && valid(s->cache.source), capture_cache_affine);

                    if (okay) {
                        if (s->storage.cache_hit((f.flags & 1) != 0, f.generation, f.content_revision, f.cache)) {
                            s->cache.texture = s->storage.textures[cache_storage].name;
                            source.performance[cache_hits]++;
                        } else {
                            s->storage.cache_valid = false;
                            uint64_t before = source.performance[blit_calls];
                            bool copied = copy_layer(*s, cache_storage, uint32_t(f.cache.texture), s->cache, s->cache.width, s->cache.height);
                            source.performance[cache_blits] += source.performance[blit_calls] - before;

                            okay = check(copied, capture_cache_copy);
                            if (copied) {
                                s->storage.copied_cache = f.cache;
                                s->storage.cache_valid = true;
                            }
                        }
                    }
                }
            } else {
                okay = okay && check(valid_absent_world(f), capture_absent_world);
                s->captured = affine{};
            }

            uint32_t logical = logical_layer_mask((f.flags & 1) != 0, f.cache.texture != 0);
            if (!(logical & 2)) s->world = {};
            if (!(logical & 8)) s->cache = {};

            s->producer = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            if (!s->producer) {
                s->storage.cache_valid = false;
                s->lease.state = lease_quarantined;
                capture_failure(capture_frame_fence, gpu_fault, &f);
                FAIL(gpu_fault);
                return;
            }

            if (!okay) s->storage.cache_valid = false;
            s->complete = okay;
            s->abandoned = !okay;
            s->lease.publish(true);
            state.source_commit++;
            g->status.source_commit = state.source_commit;
            if (okay) g->cache = f.cache;
            if (!okay) FAIL(malformed);
        }

        void native_marker(const native_frame_packet& p) {
            if (p.session != state.session || !source_ready || source_released || !source.own()) return;

            state.native_ordered_frame = p.source_frame;
            state.native_ordered_restore_serial = p.restore_serial;

            if (handoff.phase() == handoff_phase::hidden_released && !routing_fence) {
                if (router.rebind_hidden() && handoff.source_bound_hidden(true, true)) {
                    hidden_floor = p.source_frame;
                    for (auto& s : slots) {
                        if (s.frame <= hidden_floor) s.abandoned = true;
                    }
                } else if (router.fault()) {
                    FAIL(router.fault());
                }
                return;
            }

            if (prepared_surface.window && !retiring_surface.window && router.routing_hidden() && !routing_fence &&
                p.width == prepared_surface.width && p.height == prepared_surface.height) {
                GLsync old_work = nullptr;
                bool rebound = router.replace_hidden(prepared_surface.window, old_work);

                if (rebound) {
                    retiring_surface = active_surface;
                    retiring_surface.source_fence = old_work;
                    active_surface = prepared_surface;
                    prepared_surface = {};
                    resize_floor = p.source_frame;
                    source_rebound_for_resize = true;

                    for (auto& s : slots) {
                        if (s.frame <= resize_floor && s.lease.state != lease_worker_owned) s.abandoned = true;
                    }
                } else {
                    if (old_work) glDeleteSync(old_work);
                    FAIL(gpu_fault);
                }
                return;
            }

            if (pending.serial && pending.operation == op_restore_routing && handoff.phase() == handoff_phase::worker_released &&
                p.restore_serial == pending.serial) {
                if (router.rebind_original(p.source_frame, pending.serial) && handoff.source_bound_original(p.source_frame, true, true)) {
                    ack((1u << 16) | (!worker_thread.joinable() ? 4096u : 0u), 0, 1, p.source_frame);
                } else if (router.fault()) {
                    FAIL(router.fault());
                }
                return;
            }

            if (p.flags & 1) return;

            bool warmup = pending.serial && pending.operation == op_prepare && !router.routing_hidden();
            bool restoring = handoff.phase() == handoff_phase::source_restored;
            if (warmup || restoring) router.arm_original({p.session, p.generation, p.content_revision, p.source_frame, p.restore_serial});
        }

        struct callback_timer {
            int64_t start = native_now();

            ~callback_timer() {
                uint64_t elapsed = uint64_t(native_now() - start);
                source.performance[owned_callbacks]++;
                source.performance[owned_callback_ns] += elapsed;
                if (elapsed > source.performance[max_owned_callback_ns]) source.performance[max_owned_callback_ns] = elapsed;
            }
        };

        void render_event(int token, void*) {
            if (process_exit.load(std::memory_order_acquire)) return;

            dispatch packet;
            bool have = dispatches.consume(token, packet);

            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock) {
                callback_drops++;
                return;
            }

            if (process_exit.load() || !started) return;
            boundary publish_on_exit;
            callback_timer timer;

            apply_inbox();
            if (inbox.activation.cancelled()) routing_fence = true;

            if (source_ready) {
                if (!source.own()) {
                    FAIL(wrong_owner);
                    return;
                }
                poll_native();
                try_native_ack();
                source_retire();
            } else {
                source_retire();
            }

            if (router.fault()) FAIL(router.fault());
            if (!have) return;

            if (packet.kind == 1) {
                pre_gui(packet.pre);
            } else if (packet.kind == 2) {
                finish_frame(packet.frame, packet.has_scene ? &packet.scene : nullptr);
            } else {
                native_marker(packet.marker);
            }
        }

        template<class Packet>
        int queue_packet(const Packet* p, uint32_t bytes, uint32_t kind, void** ticket, int* token) {
            if (!ticket || !token) return malformed;

            *ticket = nullptr;
            *token = 0;

            if (!on_main_thread() || !p || bytes != sizeof(*p) || p->size != bytes || p->version != (kind == 2 ? 4u : 1u) ||
                !p->session || !p->source_frame) {
                return malformed;
            }
            if (p->session != active_session.load(std::memory_order_acquire)) return malformed;
            if (process_exit.load(std::memory_order_acquire) || !accepting_packets.load(std::memory_order_acquire)) return 1;

            dispatch d;
            d.kind = kind;
            d.generation = p->generation;

            if constexpr (std::is_same_v<Packet, pre_gui_packet>) {
                if (!valid_pre_gui_flags(*p)) return malformed;
                d.pre = *p;
            } else if constexpr (std::is_same_v<Packet, frame_packet>) {
                if (!p->hud_texture || p->hud_texture > UINT32_MAX || p->world_texture > UINT32_MAX || p->cache.texture > UINT32_MAX ||
                    p->hud_texture == p->world_texture || (p->flags & ~31u)) {
                    return malformed;
                }
                d.frame = *p;
                if (p->scene_description) {
                    const auto result = read_scene_frame(*p, d.scene);
                    if (result != scene_admission::ready)
                        return result == scene_admission::unsupported ? unsupported : malformed;
                    d.has_scene = true;
                    d.frame.scene_description = 0;
                    d.frame.cache = {};
                }
            } else {
                d.marker = *p;
            }

            return dispatches.queue(d, ticket, token);
        }

        int apply_command(const command_packet* p) {
            if (p->session != state.session || p->serial <= state.last_ack_serial) return malformed;

            if (pending.serial) {
                bool newer_prepare = is_prepare(pending.operation) && is_prepare(p->operation) && p->content_revision > pending.content_revision;
                if (p->operation != op_restore_routing && !newer_prepare) return 1;
                // Earlier generations and their leases stay in the ledger.
                if (auto* old = find_generation(pending.generation)) old->status.flags |= 4096;
            }

            pending = *p;
            acknowledgement = {};

            if (state.result < 0 && (is_prepare(p->operation) || p->operation == op_activate)) {
                ack(0, state.result, 3);
                return 0;
            }

            if (is_prepare(p->operation)) {
                if (p->width < 1 || p->height < 1 || p->width > 16384 || p->height > 16384 ||
                    !p->content_revision || p->content_revision > state.content_fence) {
                    ack(0, malformed, 3);
                    return 0;
                }

                // The main-thread inbox may have accepted this before a newer fence.
                reject_obsolete_preparation();
                if (!pending.serial) return 0;

                generation_state* g = find_generation(p->generation);
                if (!g) {
                    for (auto& candidate : generations) {
                        if (!candidate.status.generation || candidate.status.state == 5) {
                            g = &candidate;
                            break;
                        }
                    }
                }

                if (!g) {
                    pending = {};
                    return 1;
                }

                *g = {};
                g->status.generation = p->generation;
                g->status.content_revision = p->content_revision;
                g->status.width = p->width;
                g->status.height = p->height;
                g->status.state = 1;
                routing_fence = false;
            } else if (p->operation == op_restore_routing) {
                routing_fence = true;
                state.operation_fence = p->serial;
                handoff.begin_restore(p->serial);

                if (!source_ready && !router.installed() && !worker_thread.joinable()) {
                    ack((1u << 16) | 4096u);
                } else if (!worker_thread.joinable() || (!worker_ready && exited.load())) {
                    handoff.worker_released_original(true, true, true, true);
                }
            } else if (p->operation == op_invalidate) {
                state.content_acknowledged = state.content_fence;
                ack(8);
            } else if (p->operation == op_retire_generation) {
                auto* g = find_generation(p->generation);
                if (!g) {
                    if (!any_resources(p->generation) && !pending_dispatch(p->generation)) ack(512 | 4096);
                } else {
                    g->retiring = true;
                    g->status.retire_requested = p->serial;

                    for (auto& s : slots) {
                        if (s.generation == p->generation && s.lease.state == lease_source_writing) s.abandoned = true;
                    }

                    if (!any_resources(p->generation) && !pending_dispatch(p->generation)) {
                        g->status.state = 5;
                        ack(512 | 4096);
                    }
                }
            } else if (p->operation == op_retire_session) {
                for (auto& g : generations) g.retiring = true;
                for (auto& s : slots) {
                    if (s.lease.state == lease_source_writing) s.abandoned = true;
                }

                if (!source_ready && !router.installed() && !any_resources() && !pending_dispatch()) {
                    source_released = true;
                    accepting_packets = false;
                    ack(512);
                }
            } else if (p->operation == op_detach && !worker_ready && !router.routing_hidden()) {
                state.active_generation = 0;
                ack(256);
            } else if (p->operation == op_stop && !worker_thread.joinable() && source_released && !any_resources() && !pending_dispatch() &&
                       !router.installed()) {
                joined = true;
                state.worker_state = 5;
                handoff.retired_without_worker(state.worker_thread == 0, true, true);
                ack(2048);
            } else if (p->operation == op_release_main || p->operation == op_release_session) {
                ack(0, malformed, 3);
            }

            return 0;
        }

    }
}

SMF_SESSION_API int64_t smf_session_clock_now() {
    return native_now();
}

SMF_SESSION_API int smf_selection_publish(const smf_selection_state* p, uint32_t bytes) {
    using namespace linux_session;
    if (!on_main_thread()) return wrong_owner;
    return selection::state().publish(p, bytes);
}

SMF_SESSION_API int64_t smf_session_clock_frequency() {
    return 1000000000;
}

SMF_SESSION_API int smf_session_quit() {
    using namespace linux_session;
    if (main_thread && !on_main_thread()) return wrong_owner;
    // Stop new callbacks first, then take the gate: a callback already past its
    // entry check may still be creating the worker thread.
    accepting_packets = false;
    router.quit();
    process_exit.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lock(gate);
    if (terminal_joined) return 0;
    if (!join_quit_worker(worker_thread, lock)) return wrong_owner;

    // Unity's display is still alive here; retained GL resources and hooks are not declared retired.
    terminal_joined = true;
    if (started) {
        state.worker_state = 5;
        publish_status(true);
    }

    return 0;
}

SMF_SESSION_API int smf_session_start(uint64_t window, uint64_t session) {
    using namespace linux_session;
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (process_exit.load()) return unsupported;
    if (!window || !session) return malformed;
    if (started && state.session == session) return 0;
    if (started && !joined) return 1;
    if (session <= session_floor) return malformed;
    // Only a joined worker with retired GPU work may restart. Event tokens live for
    // the process, so a callback from an earlier session stays harmless.
    if (started && window != xid) return unsupported;
    if (router.installed() || any_resources() || !inbox.reset()) return 1;

    source = source_glx{};
    worker = worker_glx{};
    activation_presentation = worker_presentation{};

    first_capture_failure = {};
    capture_failure_mailbox.publish_boundary(first_capture_failure, native_now());
    first_failure = {};
    failure_mailbox.publish_boundary(first_failure, native_now());
    performance = {};
    performance_mailbox.publish_boundary(performance, native_now());

    for (auto& g : generations) g = {};
    for (auto& s : slots) s = slot{};

    source_ready = false;
    worker_ready = false;
    source_released = false;
    stopping = false;
    joined = false;
    routing_fence = false;
    visible = -1;
    activation_serial = 0;
    activation_key = {};
    pending = {};
    acknowledgement = {};
    exited.store(false);

    main_thread = native_thread();
    xid = window;
    session_floor = session;
    state = empty_status();
    state.session = session;
    state.main_thread = main_thread;
    started = true;

    reset_swap_trace(session);
    active_session = session;
    accepting_packets = true;
    requested_content = 0;

    handoff.reset(window, session);
    last_native = {};
    active_surface = {};
    prepared_surface = {};
    retiring_surface = {};
    ownership_drain = nullptr;
    worker_bound = false;
    resize_pending = false;
    source_rebound_for_resize = false;
    has_deferred = false;
    hidden_floor = 0;
    resize_floor = 0;
    callback_drops = 0;
    platform = {};

    publish_status(true);
    return 0;
}

SMF_SESSION_API int smf_session_content_fence(uint64_t session, uint64_t content) {
    using namespace linux_session;
    if (!on_main_thread()) return wrong_owner;
    if (session != active_session.load() || !content) return malformed;

    uint64_t previous = requested_content.load();
    if (content < previous) return malformed;
    requested_content.store(content, std::memory_order_release);
    return 0;
}

SMF_SESSION_API int smf_session_command(const linux_session::command_packet* p, uint32_t bytes) {
    using namespace linux_session;
    if (!on_main_thread() || !p || bytes != 88 || p->size != 88 || p->version != 1 || !p->serial || p->operation > op_stop ||
        p->session != active_session.load()) {
        return malformed;
    }
    return inbox.try_accept(*p);
}

SMF_SESSION_API int smf_session_ack(uint64_t session, uint64_t serial, linux_session::ack_packet* p, uint32_t bytes) {
    using namespace linux_session;
    if (!on_main_thread() || !p || bytes != 80) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;
    ack_packet value{};

    if (!ack_mailbox.read(value, publication, stamp) || value.session != session || value.serial != serial) return 1;
    *p = value;
    return 0;
}

SMF_SESSION_API int smf_session_status(linux_session::status_packet* p, uint32_t bytes) {
    using namespace linux_session;
    if (!p || bytes != 472) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;

    if (status_mailbox.read(*p, publication, stamp)) return 0;
    if (active_session.load() == 0) {
        *p = empty_status();
        return 0;
    }

    return 1;
}

SMF_SESSION_API int smf_session_platform_status(linux_session::platform_status* p, uint32_t bytes) {
    using namespace linux_session;
    if (!p || bytes != sizeof(*p)) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;

    if (!platform_mailbox.read(*p, publication, stamp)) return 1;
    p->publication = publication;
    p->published_ns = stamp;
    return 0;
}

SMF_SESSION_API int smf_session_performance(linux_session::performance_status* p, uint32_t bytes) {
    using namespace linux_session;
    if (!p || bytes != sizeof(*p)) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;

    if (!performance_mailbox.read(*p, publication, stamp) || !p->session) return 1;
    p->publication = publication;
    p->published_ns = stamp;
    return 0;
}

SMF_SESSION_API int smf_session_failure_diagnostic(linux_session::failure_diagnostic* p, uint32_t bytes) {
    using namespace linux_session;
    if (!p || bytes != sizeof(*p)) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;

    if (!failure_mailbox.read(*p, publication, stamp) || !p->line) return 1;
    return 0;
}

SMF_SESSION_API int smf_session_capture_diagnostic(linux_session::capture_diagnostic* p, uint32_t bytes) {
    using namespace linux_session;
    if (!p || bytes != sizeof(*p)) return malformed;

    uint64_t publication = 0;
    int64_t stamp = 0;

    if (!capture_failure_mailbox.read(*p, publication, stamp) || !p->stage) return 1;
    return 0;
}

SMF_SESSION_API int smf_session_pre_gui(const linux_session::pre_gui_packet* p, uint32_t bytes, void** ticket, int* token) {
    return linux_session::queue_packet(p, bytes, 1, ticket, token);
}

SMF_SESSION_API int smf_session_frame(const linux_session::frame_packet* p, uint32_t bytes, void** ticket, int* token) {
    return linux_session::queue_packet(p, bytes, 2, ticket, token);
}

SMF_SESSION_API int smf_session_native_frame(const linux_session::native_frame_packet* p, uint32_t bytes, void** ticket, int* token) {
    return linux_session::queue_packet(p, bytes, 3, ticket, token);
}

SMF_SESSION_API int smf_session_cancel(void* ticket, int token) {
    using namespace linux_session;
    if (!on_main_thread()) return wrong_owner;
    return dispatches.cancel(ticket, token);
}

SMF_SESSION_API void* smf_session_render_event() {
    return reinterpret_cast<void*>(&linux_session::render_event);
}

SMF_SESSION_API int smf_session_poll_joined(uint64_t session) {
    using namespace linux_session;
    if (!on_main_thread()) return wrong_owner;
    std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
    if (!lock) return 1;
    if (session != state.session) return malformed;

    // With no worker running and no callback promised, main consumes the accepted stop itself.
    if (!process_exit.load()) {
        bool worker_live = worker_ready || (worker_thread.joinable() && !exited.load());
        if (can_pump_stopped(source_released, worker_live, any_resources(), pending_dispatch(), router.installed())) {
            apply_inbox();
            publish_status(true);
        }
    }

    if (joined) return 0;
    if (!exited.load(std::memory_order_acquire)) return 1;
    // An exited worker does not retire source storage.
    if (!source_released || any_resources() || pending_dispatch() || router.installed()) return 1;

    if (worker_thread.joinable()) worker_thread.join();
    joined = true;
    state.worker_state = 5;
    if (pending.operation == op_stop && pending.serial) ack(2048);
    handoff.retired(!any_resources(), !router.installed(), true);
    publish_status(true);
    return 0;
}
