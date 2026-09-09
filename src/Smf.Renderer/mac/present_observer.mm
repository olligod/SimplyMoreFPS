#include "present_observer.h"
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <cstdio>
#include <mach/mach_time.h>
#include <pthread.h>

namespace present_observer {
    struct hook;
    struct observer_state;
}

// The associated object pins the original IMP owner even when a call was
// dispatched right before main restores the instance class.
@interface SmfPresentHookBox : NSObject {
@public
    std::shared_ptr<present_observer::hook> entry;
}
@end
@implementation SmfPresentHookBox
@end

namespace present_observer {
    namespace {

        enum hook_kind : uint32_t { layer_hook = 1, queue_hook, buffer_hook, texture_hook, drawable_hook, blit_hook };
        constexpr size_t hook_limit = 256;
        constexpr size_t class_limit = 48;
        char association_key;

        uint64_t address_of(id object) {
            return uint64_t((__bridge void*)object);
        }

        uint64_t now_ns() {
            static const auto timebase = [] {
                mach_timebase_info_data_t value{};
                mach_timebase_info(&value);
                return value;
            }();
            return uint64_t((long double)mach_absolute_time() * timebase.numer / timebase.denom);
        }

        uint64_t current_thread() {
            uint64_t value = 0;
            pthread_threadid_np(nullptr, &value);
            return value;
        }

    }

    struct saved_methods {
        std::array<std::pair<SEL, IMP>, 32> entries{};
        uint32_t count = 0;
    };

    struct hook {
        std::shared_ptr<observer_state> state;
        __weak id object = nil;
        Class original = Nil;
        Class subclass = Nil;
        const saved_methods* methods = nullptr;
        uint32_t kind = 0;
        uint64_t identity = 0;
        uint64_t parent = 0;
        std::atomic<uint64_t> epoch{0};
        ~hook();
    };

    struct hook_slot {
        __weak SmfPresentHookBox* box = nil;
    };

    struct gpu_lease {
        uint64_t submission_id = 0;
        __strong id<MTLCommandBuffer> command = nil;
        __strong id<MTLTexture> source[4]{nil, nil, nil, nil};
    };

    struct observer_state {
        explicit observer_state(uint64_t session) : model(session) {}

        std::mutex model_mutex;
        std::mutex hook_mutex;
        present_model model;
        __strong CAMetalLayer* layer = nil;
        __strong id<MTLCommandQueue> queue = nil;
        std::atomic<uint64_t> queue_address{0};
        std::atomic<uint64_t> next_identity{0};
        uint64_t layer_address = 0;
        uint64_t device_address = 0;
        uint32_t pixel_format = 0;
        std::atomic<uint32_t> calls{0};
        std::atomic<uint32_t> live_hooks{0};
        std::atomic<bool> stopping{false};
        std::atomic<bool> removed{false};
        std::array<hook_slot, hook_limit> hooks{};
        std::array<gpu_lease, present_model::buffer_limit> leases{};
        uint64_t source_failures = 0;
        uint64_t class_restore_conflicts = 0;
        uint64_t source_thread = 0;
        source_rejection first_rejection{};
    };

    hook::~hook() {
        if (state) state->live_hooks.fetch_sub(1, std::memory_order_relaxed);
    }

    namespace {

        std::mutex current_mutex;
        std::mutex class_mutex;
        std::shared_ptr<observer_state> active;

        struct class_entry {
            Class original = Nil;
            Class subclass = Nil;
            uint32_t kind = 0;
            saved_methods methods;
        };
        std::array<class_entry, class_limit> classes{};

        thread_local uint64_t scheduled_submission = 0;
        thread_local uint64_t direct_present_drawable = 0;
        thread_local observer_state* scheduled_state = nullptr;
        thread_local unsigned factory_depth = 0;
        thread_local observer_state* queue_call_state = nullptr;
        thread_local uint64_t queue_call_submission = 0;

        std::shared_ptr<observer_state> current() {
            std::lock_guard<std::mutex> lock(current_mutex);
            return active;
        }

        std::shared_ptr<hook> hook_of(id object) {
            SmfPresentHookBox* box = object ? objc_getAssociatedObject(object, &association_key) : nil;
            return box ? box->entry : std::shared_ptr<hook>{};
        }

        // Counts one in-flight dispatch on the hooked object's observer.
        struct hooked_call {
            std::shared_ptr<hook> h;

            explicit hooked_call(id object) : h(hook_of(object)) {
                if (h) h->state->calls.fetch_add(1, std::memory_order_acq_rel);
            }

            ~hooked_call() {
                if (h) h->state->calls.fetch_sub(1, std::memory_order_acq_rel);
            }
        };

        template<class F> F original_imp(const std::shared_ptr<hook>& h, SEL sel) {
            for (uint32_t i = 0; i < h->methods->count; ++i) {
                if (h->methods->entries[i].first == sel) return reinterpret_cast<F>(h->methods->entries[i].second);
            }

            // Only selectors recorded while creating this exact subclass dispatch here.
            std::terminate();
        }

        void fail(const std::shared_ptr<observer_state>& s, fault_kind why) {
            std::lock_guard<std::mutex> lock(s->model_mutex);
            s->model.fail(why);
        }

        void add_methods(Class subclass, Class original, uint32_t kind);

        Class make_subclass(Class original, uint32_t kind, const saved_methods*& saved) {
            std::lock_guard<std::mutex> lock(class_mutex);

            for (const auto& entry : classes) {
                if (entry.original == original && entry.kind == kind) {
                    saved = &entry.methods;
                    return entry.subclass;
                }
            }

            for (auto& entry : classes) {
                if (entry.original) continue;

                char name[96];
                snprintf(name, sizeof(name), "SmfPresentObserver_%u_%llx", kind, (unsigned long long)(uintptr_t)original);
                // An unrelated class bearing our name is not safe to adopt.
                if (objc_getClass(name)) return Nil;

                Class subclass = objc_allocateClassPair(original, name, 0);
                if (!subclass) return Nil;
                add_methods(subclass, original, kind);

                unsigned count = 0;
                Method* methods = class_copyMethodList(subclass, &count);
                if (count > entry.methods.entries.size()) {
                    free(methods);
                    objc_disposeClassPair(subclass);
                    return Nil;
                }

                entry.methods.count = count;
                for (unsigned i = 0; i < count; ++i) {
                    SEL sel = method_getName(methods[i]);
                    entry.methods.entries[i] = {sel, class_getMethodImplementation(original, sel)};
                }
                free(methods);
                objc_registerClassPair(subclass);

                entry.original = original;
                entry.subclass = subclass;
                entry.kind = kind;
                saved = &entry.methods;
                return subclass;
            }

            return Nil;
        }

        std::shared_ptr<hook> attach(const std::shared_ptr<observer_state>& s, id object, uint32_t kind, uint64_t parent = 0) {
            if (!object || s->stopping.load(std::memory_order_acquire)) return {};

            std::lock_guard<std::mutex> lock(s->hook_mutex);
            if (auto old = hook_of(object)) {
                if (old->state == s) {
                    if (old->kind == kind && old->parent == parent) return old;
                    fail(s, fault_kind::identity);
                    return {};
                }

                // A removed prior session keeps its association only to protect already
                // dispatched trampolines. Replacing it here is safe after class restore.
                if (!old->state->removed.load() || object_getClass(object) != old->original) {
                    fail(s, fault_kind::class_conflict);
                    return {};
                }
            }

            hook_slot* slot = nullptr;
            for (auto& candidate : s->hooks) {
                if (!candidate.box) {
                    slot = &candidate;
                    break;
                }
            }

            if (!slot || s->live_hooks.load() >= hook_limit) {
                fail(s, fault_kind::pool);
                return {};
            }

            auto h = std::make_shared<hook>();
            h->state = s;
            h->object = object;
            h->kind = kind;
            h->parent = parent;
            h->identity = s->next_identity.fetch_add(1) + 1;
            h->original = object_getClass(object);
            s->live_hooks.fetch_add(1);

            if (kind != texture_hook) {
                h->subclass = make_subclass(h->original, kind, h->methods);
                if (!h->subclass) {
                    fail(s, fault_kind::class_conflict);
                    return {};
                }
            }

            SmfPresentHookBox* box = [SmfPresentHookBox new];
            box->entry = h;
            objc_setAssociatedObject(object, &association_key, box, OBJC_ASSOCIATION_RETAIN);
            slot->box = box;

            if (h->subclass) {
                if (object_getClass(object) != h->original) {
                    fail(s, fault_kind::class_conflict);
                    return {};
                }
                object_setClass(object, h->subclass);
            }

            return h;
        }

        Class reported_class(id self, SEL) {
            hooked_call c(self);
            return c.h->original;
        }

        std::shared_ptr<hook> attach_texture(const std::shared_ptr<observer_state>& s, id<MTLTexture> texture) {
            return attach(s, texture, texture_hook);
        }

        uint64_t acquisition_for_texture(const std::shared_ptr<observer_state>& s, uint64_t texture) {
            for (const auto& d : s->model.drawables) {
                if (d.id && d.texture == texture) return d.id;
            }
            return 0;
        }

        void observe_geometry(const std::shared_ptr<observer_state>& s) {
            if (s->stopping.load() || !s->layer) return;
            const CGSize size = s->layer.drawableSize;
            const auto format = s->layer.pixelFormat;

            std::lock_guard<std::mutex> lock(s->model_mutex);
            s->model.set_geometry(uint32_t(size.width), uint32_t(size.height), uint32_t(format));
        }

        void complete(const std::shared_ptr<observer_state>& s, uint64_t submission_id, uint64_t object, id<MTLCommandBuffer> actual) {
            const bool good = actual && actual.status == MTLCommandBufferStatusCompleted && !actual.error &&
                address_of(actual.commandQueue) == s->queue_address.load(std::memory_order_acquire);

            std::lock_guard<std::mutex> lock(s->model_mutex);
            for (auto& lease : s->leases) {
                if (lease.submission_id != submission_id) continue;
                if (lease.command != actual) {
                    s->model.fail(fault_kind::identity);
                    return;
                }
                lease = gpu_lease{};
                s->model.complete(submission_id, object, good);
                return;
            }

            // An old completion never clears a newer lease for a reused object. Without
            // an exact lease nothing is completed or published.
            ++s->model.counters.stale_callbacks;
        }

        std::shared_ptr<hook> track_buffer(const std::shared_ptr<observer_state>& s, id<MTLCommandBuffer> cb, bool fresh) {
            if (!cb || s->stopping.load() || address_of(cb.commandQueue) != s->queue_address.load()) return {};

            auto h = attach(s, cb, buffer_hook);
            if (!h) return {};
            uint64_t submission_id = 0;

            {
                std::lock_guard<std::mutex> lock(s->model_mutex);
                submission_id = s->model.register_buffer(h->identity, fresh, uint32_t(cb.status));
                if (!submission_id) return {};
                if (submission_id == h->epoch.load()) return h;

                gpu_lease* free_lease = nullptr;
                for (auto& lease : s->leases) {
                    if (!lease.submission_id) {
                        free_lease = &lease;
                        break;
                    }
                }

                if (!free_lease) {
                    s->model.fail(fault_kind::pool);
                    return {};
                }

                free_lease->submission_id = submission_id;
                free_lease->command = cb;
                h->epoch.store(submission_id);
            }

            // The handler retains no command buffer or drawable. The bounded lease
            // table owns the source and command buffer until this exact completion
            // runs. Owning locals keep the escaping block independent of the caller's
            // const reference parameter.
            const std::shared_ptr<observer_state> owner = s;
            const uint64_t object = h->identity;

            [cb addCompletedHandler:^(id<MTLCommandBuffer> actual) {
                complete(owner, submission_id, object, actual);
            }];
            return h;
        }

        id queue_command_buffer(id self, SEL sel) {
            hooked_call c(self);
            id result = nil;
            ++factory_depth;

            @try {
                result = original_imp<id (*)(id, SEL)>(c.h, sel)(self, sel);
            } @finally {
                --factory_depth;
            }

            if (!factory_depth) track_buffer(c.h->state, result, true);
            return result;
        }

        id queue_command_buffer_with_descriptor(id self, SEL sel, id descriptor) {
            hooked_call c(self);
            id result = nil;
            ++factory_depth;

            @try {
                result = original_imp<id (*)(id, SEL, id)>(c.h, sel)(self, sel, descriptor);
            } @finally {
                --factory_depth;
            }

            if (!factory_depth) track_buffer(c.h->state, result, true);
            return result;
        }

        void queue_call(id self, SEL sel) {
            hooked_call c(self);
            auto s = c.h->state;
            const auto submission_id = c.h->epoch.load();
            bool began = false;
            const bool nested = queue_call_state == s.get() && queue_call_submission == submission_id;

            if (!s->stopping.load() && !nested) {
                const uint32_t status = uint32_t(((id<MTLCommandBuffer>)self).status);
                std::lock_guard<std::mutex> lock(s->model_mutex);
                began = s->model.begin_queue(submission_id, sel == @selector(enqueue), status);
            }

            auto* previous_state = queue_call_state;
            const auto previous_submission = queue_call_submission;
            queue_call_state = s.get();
            queue_call_submission = submission_id;

            @try {
                original_imp<void (*)(id, SEL)>(c.h, sel)(self, sel);
            } @finally {
                queue_call_state = previous_state;
                queue_call_submission = previous_submission;
                if (began) {
                    const uint32_t status = uint32_t(((id<MTLCommandBuffer>)self).status);
                    std::lock_guard<std::mutex> lock(s->model_mutex);
                    s->model.end_queue(submission_id, true, status);
                }
            }
        }

        void add_scheduled_handler(id self, SEL sel, void (^block)(id<MTLCommandBuffer>)) {
            hooked_call c(self);
            auto s = c.h->state;
            const auto submission_id = c.h->epoch.load();
            const auto buffer_address = address_of(self);
            void (^wrapped)(id<MTLCommandBuffer>) = nil;

            if (block) {
                wrapped = ^(id<MTLCommandBuffer> actual) {
                    const auto previous_submission = scheduled_submission;
                    auto* previous_state = scheduled_state;
                    scheduled_submission = address_of(actual) == buffer_address && address_of(actual.commandQueue) == s->queue_address.load() ? submission_id : 0;
                    scheduled_state = s.get();

                    @try {
                        block(actual);
                    } @finally {
                        scheduled_submission = previous_submission;
                        scheduled_state = previous_state;
                    }
                };
            }

            original_imp<void (*)(id, SEL, void (^)(id<MTLCommandBuffer>))>(c.h, sel)(self, sel, wrapped);
        }

        id next_drawable(id self, SEL sel) {
            hooked_call c(self);
            auto s = c.h->state;
            id<CAMetalDrawable> drawable = original_imp<id (*)(id, SEL)>(c.h, sel)(self, sel);

            if (!drawable || s->stopping.load()) return drawable;
            if (drawable.layer != (CAMetalLayer*)self || drawable.texture.device != ((CAMetalLayer*)self).device) {
                fail(s, fault_kind::identity);
                return drawable;
            }

            observe_geometry(s);
            auto texture_entry = attach_texture(s, drawable.texture);
            auto drawable_entry = attach(s, drawable, drawable_hook);
            if (!texture_entry || !drawable_entry) return drawable;

            uint64_t acquisition_id = 0;
            {
                std::lock_guard<std::mutex> lock(s->model_mutex);
                acquisition_id = s->model.acquire(drawable_entry->identity, texture_entry->identity, uint32_t(drawable.texture.width),
                    uint32_t(drawable.texture.height), uint32_t(drawable.texture.pixelFormat), now_ns(), address_of(drawable));
                drawable_entry->epoch.store(acquisition_id);
            }

            if (!acquisition_id) return drawable;
            const uint64_t drawable_address = address_of(drawable);
            const uint64_t object = drawable_entry->identity;

            MTLDrawablePresentedHandler callback = ^(id<MTLDrawable> actual) {
                const double actual_time = address_of(actual) == drawable_address ? actual.presentedTime : 0;
                observe_geometry(s);
                std::lock_guard<std::mutex> lock(s->model_mutex);
                s->model.presented(acquisition_id, object, actual_time, now_ns());
            };
            [drawable addPresentedHandler:callback];
            return drawable;
        }

        void observe_present(const std::shared_ptr<hook>& h) {
            auto s = h->state;
            if (s->stopping.load() || direct_present_drawable == address_of(h->object)) return;

            observe_geometry(s);
            std::lock_guard<std::mutex> lock(s->model_mutex);
            s->model.present(h->epoch.load(), h->identity, scheduled_state == s.get() ? scheduled_submission : 0);
        }

        void present(id self, SEL sel) {
            hooked_call c(self);
            observe_present(c.h);
            original_imp<void (*)(id, SEL)>(c.h, sel)(self, sel);
        }

        void present_at_time(id self, SEL sel, double time) {
            hooked_call c(self);
            observe_present(c.h);
            original_imp<void (*)(id, SEL, double)>(c.h, sel)(self, sel, time);
        }

        void observe_buffer_present(const std::shared_ptr<hook>& buffer_entry, id drawable) {
            auto drawable_entry = hook_of(drawable);
            if (!drawable_entry || drawable_entry->kind != drawable_hook || drawable_entry->state != buffer_entry->state ||
                buffer_entry->state->stopping.load()) return;

            observe_geometry(buffer_entry->state);
            std::lock_guard<std::mutex> lock(buffer_entry->state->model_mutex);
            buffer_entry->state->model.present(drawable_entry->epoch.load(), drawable_entry->identity, buffer_entry->epoch.load(), true);
        }

        void buffer_present_drawable(id self, SEL sel, id drawable) {
            hooked_call c(self);
            observe_buffer_present(c.h, drawable);
            const auto previous = direct_present_drawable;
            direct_present_drawable = address_of(drawable);

            @try {
                original_imp<void (*)(id, SEL, id)>(c.h, sel)(self, sel, drawable);
            } @finally {
                direct_present_drawable = previous;
            }
        }

        void buffer_present_drawable_at_time(id self, SEL sel, id drawable, double time) {
            hooked_call c(self);
            observe_buffer_present(c.h, drawable);
            const auto previous = direct_present_drawable;
            direct_present_drawable = address_of(drawable);

            @try {
                original_imp<void (*)(id, SEL, id, double)>(c.h, sel)(self, sel, drawable, time);
            } @finally {
                direct_present_drawable = previous;
            }
        }

        void mark_write(const std::shared_ptr<observer_state>& s, uint64_t submission_id, id<MTLTexture> texture) {
            if (!texture) return;
            auto texture_entry = attach_texture(s, texture);
            if (!texture_entry) return;

            const uint64_t texture_id = texture_entry->identity;
            std::lock_guard<std::mutex> lock(s->model_mutex);
            if (auto acquisition_id = acquisition_for_texture(s, texture_id)) {
                s->model.target_write(submission_id, texture_id, acquisition_id);
            } else {
                s->model.write(submission_id, texture_id);
            }
        }

        id render_command_encoder(id self, SEL sel, MTLRenderPassDescriptor* pass) {
            hooked_call c(self);
            auto s = c.h->state;
            id encoder = original_imp<id (*)(id, SEL, MTLRenderPassDescriptor*)>(c.h, sel)(self, sel, pass);

            if (encoder && !s->stopping.load()) {
                for (NSUInteger i = 0; i < 8; ++i) {
                    auto attachment = pass.colorAttachments[i];
                    mark_write(s, c.h->epoch.load(), attachment.texture);
                    if (attachment.resolveTexture) mark_write(s, c.h->epoch.load(), attachment.resolveTexture);
                }
            }
            return encoder;
        }

        id blit_command_encoder(id self, SEL sel) {
            hooked_call c(self);
            id encoder = original_imp<id (*)(id, SEL)>(c.h, sel)(self, sel);
            attach(c.h->state, encoder, blit_hook, c.h->epoch.load());
            return encoder;
        }

        id blit_command_encoder_with_descriptor(id self, SEL sel, id descriptor) {
            hooked_call c(self);
            id encoder = original_imp<id (*)(id, SEL, id)>(c.h, sel)(self, sel, descriptor);
            attach(c.h->state, encoder, blit_hook, c.h->epoch.load());
            return encoder;
        }

        bool plain_texture(id<MTLTexture> texture) {
            return texture && texture.textureType == MTLTextureType2D && texture.sampleCount == 1 && texture.arrayLength == 1;
        }

        void observe_copy(const std::shared_ptr<hook>& h, id<MTLTexture> source, id<MTLTexture> destination,
            NSUInteger width, NSUInteger height, bool exact) {
            auto s = h->state;
            if (s->stopping.load()) return;

            auto source_entry = attach_texture(s, source);
            auto destination_entry = attach_texture(s, destination);
            const uint64_t source_id = source_entry ? source_entry->identity : 0;
            const uint64_t destination_id = destination_entry ? destination_entry->identity : 0;
            if (!source_id && !destination_id) return;

            const bool metadata = plain_texture(source) && plain_texture(destination) && source.device == destination.device &&
                address_of(source.device) == s->device_address && source.pixelFormat == destination.pixelFormat &&
                source.width == width && source.height == height && destination.width == width && destination.height == height;

            std::lock_guard<std::mutex> lock(s->model_mutex);
            const uint64_t acquisition_id = acquisition_for_texture(s, destination_id);
            if (acquisition_id) {
                if (exact && metadata && s->model.watched(source_id) && !s->model.drawable(acquisition_id)->final_buffer) {
                    s->model.full_copy(h->parent, source_id, destination_id, acquisition_id, uint32_t(width), uint32_t(height),
                        uint32_t(source.pixelFormat), true);
                } else {
                    s->model.target_write(h->parent, destination_id, acquisition_id);
                }
            } else if (destination_id) {
                s->model.write(h->parent, destination_id);
            }
        }

        void copy_texture_region(id self, SEL sel, id<MTLTexture> source, NSUInteger slice, NSUInteger level, MTLOrigin origin, MTLSize extent,
            id<MTLTexture> destination, NSUInteger dest_slice, NSUInteger dest_level, MTLOrigin dest_origin) {
            hooked_call c(self);
            observe_copy(c.h, source, destination, extent.width, extent.height,
                !slice && !level && !dest_slice && !dest_level && !origin.x && !origin.y && !origin.z &&
                !dest_origin.x && !dest_origin.y && !dest_origin.z && extent.depth == 1);
            original_imp<void (*)(id, SEL, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin)>(c.h, sel)
                (self, sel, source, slice, level, origin, extent, destination, dest_slice, dest_level, dest_origin);
        }

        void copy_texture(id self, SEL sel, id<MTLTexture> source, id<MTLTexture> destination) {
            hooked_call c(self);
            observe_copy(c.h, source, destination, source.width, source.height, source.mipmapLevelCount == 1 && destination.mipmapLevelCount == 1);
            original_imp<void (*)(id, SEL, id<MTLTexture>, id<MTLTexture>)>(c.h, sel)(self, sel, source, destination);
        }

        void copy_buffer_to_texture(id self, SEL sel, id<MTLBuffer> source, NSUInteger offset, NSUInteger row, NSUInteger image, MTLSize size,
            id<MTLTexture> destination, NSUInteger slice, NSUInteger level, MTLOrigin origin) {
            hooked_call c(self);
            mark_write(c.h->state, c.h->parent, destination);
            original_imp<void (*)(id, SEL, id<MTLBuffer>, NSUInteger, NSUInteger, NSUInteger, MTLSize, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin)>(c.h, sel)
                (self, sel, source, offset, row, image, size, destination, slice, level, origin);
        }

        void copy_buffer_to_texture_with_options(id self, SEL sel, id<MTLBuffer> source, NSUInteger offset, NSUInteger row, NSUInteger image, MTLSize size,
            id<MTLTexture> destination, NSUInteger slice, NSUInteger level, MTLOrigin origin, MTLBlitOption options) {
            hooked_call c(self);
            mark_write(c.h->state, c.h->parent, destination);
            original_imp<void (*)(id, SEL, id<MTLBuffer>, NSUInteger, NSUInteger, NSUInteger, MTLSize, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin, MTLBlitOption)>(c.h, sel)
                (self, sel, source, offset, row, image, size, destination, slice, level, origin, options);
        }

        void copy_texture_range(id self, SEL sel, id<MTLTexture> source, NSUInteger source_slice, NSUInteger source_level, id<MTLTexture> destination,
            NSUInteger destination_slice, NSUInteger destination_level, NSUInteger slice_count, NSUInteger level_count) {
            hooked_call c(self);
            observe_copy(c.h, source, destination, source.width, source.height,
                !source_slice && !source_level && !destination_slice && !destination_level && slice_count == 1 && level_count == 1);
            original_imp<void (*)(id, SEL, id<MTLTexture>, NSUInteger, NSUInteger, id<MTLTexture>, NSUInteger, NSUInteger, NSUInteger, NSUInteger)>(c.h, sel)
                (self, sel, source, source_slice, source_level, destination, destination_slice, destination_level, slice_count, level_count);
        }

        void generate_mipmaps(id self, SEL sel, id<MTLTexture> texture) {
            hooked_call c(self);
            mark_write(c.h->state, c.h->parent, texture);
            original_imp<void (*)(id, SEL, id<MTLTexture>)>(c.h, sel)(self, sel, texture);
        }

        // Unrelated compute work does not invalidate a presented frame. A compute-only
        // path without observed target work stays unavailable.
        id unsupported_encoder(id self, SEL sel) {
            hooked_call c(self);
            return original_imp<id (*)(id, SEL)>(c.h, sel)(self, sel);
        }

        id unsupported_encoder_with_descriptor(id self, SEL sel, id descriptor) {
            hooked_call c(self);
            return original_imp<id (*)(id, SEL, id)>(c.h, sel)(self, sel, descriptor);
        }

        id unsupported_compute_encoder(id self, SEL sel, MTLDispatchType type) {
            hooked_call c(self);
            return original_imp<id (*)(id, SEL, MTLDispatchType)>(c.h, sel)(self, sel, type);
        }

        void add_method(Class subclass, Class original, SEL selector, IMP implementation) {
            Method method = class_getInstanceMethod(original, selector);
            if (method) class_addMethod(subclass, selector, implementation, method_getTypeEncoding(method));
        }

        void add_methods(Class subclass, Class original, uint32_t kind) {
#define HOOK(selector_name, implementation) add_method(subclass, original, @selector(selector_name), (IMP)implementation)
            HOOK(class, reported_class);
            if (kind == layer_hook) {
                HOOK(nextDrawable, next_drawable);
            } else if (kind == queue_hook) {
                HOOK(commandBuffer, queue_command_buffer);
                HOOK(commandBufferWithUnretainedReferences, queue_command_buffer);
                HOOK(commandBufferWithDescriptor:, queue_command_buffer_with_descriptor);
            } else if (kind == buffer_hook) {
                HOOK(enqueue, queue_call);
                HOOK(commit, queue_call);
                HOOK(addScheduledHandler:, add_scheduled_handler);
                HOOK(presentDrawable:, buffer_present_drawable);
                HOOK(presentDrawable:atTime:, buffer_present_drawable_at_time);
                HOOK(presentDrawable:afterMinimumDuration:, buffer_present_drawable_at_time);
                HOOK(renderCommandEncoderWithDescriptor:, render_command_encoder);
                HOOK(blitCommandEncoder, blit_command_encoder);
                HOOK(blitCommandEncoderWithDescriptor:, blit_command_encoder_with_descriptor);
                HOOK(computeCommandEncoder, unsupported_encoder);
                HOOK(computeCommandEncoderWithDescriptor:, unsupported_encoder_with_descriptor);
                HOOK(computeCommandEncoderWithDispatchType:, unsupported_compute_encoder);
                HOOK(parallelRenderCommandEncoderWithDescriptor:, render_command_encoder);
            } else if (kind == drawable_hook) {
                HOOK(present, present);
                HOOK(presentAtTime:, present_at_time);
                HOOK(presentAfterMinimumDuration:, present_at_time);
            } else if (kind == blit_hook) {
                HOOK(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:, copy_texture_region);
                HOOK(copyFromTexture:toTexture:, copy_texture);
                HOOK(copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:, copy_texture_range);
                HOOK(copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:, copy_buffer_to_texture);
                HOOK(copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:, copy_buffer_to_texture_with_options);
                HOOK(generateMipmapsForTexture:, generate_mipmaps);
            }
#undef HOOK
        }

        source_reject retain_source(const std::shared_ptr<observer_state>& s, uint64_t submission_id, id<MTLTexture> source) {
            std::lock_guard<std::mutex> lock(s->model_mutex);

            for (auto& lease : s->leases) {
                if (lease.submission_id != submission_id) continue;
                for (auto texture : lease.source) {
                    if (texture == source) return source_reject::none;
                }

                for (auto& texture : lease.source) {
                    if (!texture) {
                        texture = source;
                        return source_reject::none;
                    }
                }

                s->model.fail(fault_kind::pool);
                return source_reject::source_array_full;
            }

            s->model.fail(fault_kind::pool);
            return source_reject::missing_gpu_lease;
        }

    }

    int install(uint64_t session, CAMetalLayer* layer) {
        if (!pthread_main_np() || !session || !layer) return -202;

        {
            auto previous = current();
            if (previous) return previous->model.session == session && previous->layer == layer ? 0 : 1;
        }

        auto s = std::make_shared<observer_state>(session);
        s->layer = layer;
        s->layer_address = address_of(layer);
        s->device_address = address_of(layer.device);
        s->pixel_format = uint32_t(layer.pixelFormat);
        if (!attach(s, layer, layer_hook)) return -204;

        std::lock_guard<std::mutex> lock(current_mutex);
        active = s;
        return 0;
    }

    int source(IUnityGraphicsMetal* metal, const mac_source_target& target, const session_native_frame& marker) {
        @autoreleasepool {
            auto s = current();
            if (!s || s->stopping.load() || !mac::native_target_matches(target, marker) || marker.session != s->model.session) return 1;

            source_rejection observation;
            observation.target = target;
            observation.restore_serial = marker.restore_serial;
            observation.original_device = s->device_address;
            observation.original_format = s->pixel_format;
            observation.original_layer = s->layer_address;

            auto reject = [&](source_reject reason) {
                observation.reason = uint32_t(reason);
                observation.observed_ns = now_ns();
                observation.render_thread = current_thread();
                std::lock_guard<std::mutex> lock(s->model_mutex);
                observation.failure_ordinal = ++s->source_failures;
                if (!s->first_rejection.reason) s->first_rejection = observation;
                return -204;
            };

            @try {
                observation.stage = 1;
                id<MTLDevice> device = metal ? metal->MetalDevice() : nil;
                observation.device = address_of(device);
                observation.properties_read |= 1;

                observation.stage = 2;
                id<MTLCommandBuffer> cb = metal ? metal->CurrentCommandBuffer() : nil;
                observation.command = address_of(cb);
                observation.properties_read |= 2;

                observation.stage = 3;
                id<MTLCommandQueue> queue = cb.commandQueue;
                observation.queue = address_of(queue);
                observation.properties_read |= 4;

                observation.stage = 4;
                MTLRenderPassDescriptor* before = metal ? metal->CurrentRenderPassDescriptor() : nil;
                auto attachment = before.colorAttachments[0];
                observation.before_pass = address_of(before);
                observation.properties_read |= 8;

                observation.stage = 5;
                id<MTLTexture> source_texture = metal ? metal->TextureFromRenderBuffer((UnityRenderBuffer)(uintptr_t)target.native_render_buffer) : nil;
                observation.source_texture = address_of(source_texture);
                observation.properties_read |= 16;

                observation.stage = 6;
                auto pass = metal ? metal->CurrentRenderPassDescriptor() : nil;
                auto current_attachment = pass.colorAttachments[0];
                observation.after_pass = address_of(pass);
                observation.properties_read |= 32;

                observation.stage = 7;
                observation.command_device = address_of(cb.device);
                observation.queue_device = address_of(queue.device);
                observation.source_device = address_of(source_texture.device);
                observation.properties_read |= 64;

                observation.stage = 8;
                observation.command_after = address_of(metal ? metal->CurrentCommandBuffer() : nil);
                observation.command_status = uint64_t(cb.status);
                observation.properties_read |= 128;

                observation.stage = 9;
                // Keep this read order: the earlier descriptor's attachment texture is
                // read after TextureFromRenderBuffer, which may mutate render state.
                observation.before_texture = address_of(attachment.texture);
                observation.after_texture = address_of(current_attachment.texture);
                observation.after_level = uint64_t(current_attachment.level);
                observation.after_slice = uint64_t(current_attachment.slice);
                observation.after_depth = uint64_t(current_attachment.depthPlane);
                observation.before_level = uint64_t(attachment.level);
                observation.before_slice = uint64_t(attachment.slice);
                observation.before_depth = uint64_t(attachment.depthPlane);
                observation.properties_read |= 256;

                observation.stage = 10;
                observation.source_type = uint64_t(source_texture.textureType);
                observation.sample_count = uint64_t(source_texture.sampleCount);
                observation.array_length = uint64_t(source_texture.arrayLength);
                observation.framebuffer_only = source_texture.framebufferOnly;
                observation.source_width = uint64_t(source_texture.width);
                observation.source_height = uint64_t(source_texture.height);
                observation.source_format = uint64_t(source_texture.pixelFormat);
                observation.mipmap_levels = uint64_t(source_texture.mipmapLevelCount);
                observation.storage_mode = uint64_t(source_texture.storageMode);
                observation.properties_read |= 512;

                observation.stage = 11;
                static_assert(MTLTextureType2D == 2, "Pure diagnostic uses public texture enum");
                const auto reason = reject_source(observation);
                if (reason != source_reject::none) return reject(reason);

                const uint64_t queue_identity = address_of(queue);
                {
                    std::lock_guard<std::mutex> lock(s->model_mutex);
                    if (!s->queue_address.load()) {
                        s->queue = queue;
                        s->queue_address.store(queue_identity, std::memory_order_release);
                    } else if (s->queue_address.load() != queue_identity) {
                        s->model.fail(fault_kind::queue_conflict);
                        return -204;
                    }

                    if (s->source_thread && s->source_thread != current_thread()) {
                        s->model.fail(fault_kind::identity);
                        return -202;
                    }
                    s->source_thread = current_thread();
                }

                observation.stage = 12;
                if (!attach(s, queue, queue_hook)) return reject(source_reject::queue_hook);
                auto buffer_entry = track_buffer(s, cb, false);
                observation.stage = 13;
                if (!buffer_entry) return reject(source_reject::track_buffer);
                auto texture_entry = attach_texture(s, source_texture);
                if (!texture_entry) return reject(source_reject::texture_hook);
                const auto retained = retain_source(s, buffer_entry->epoch.load(), source_texture);
                if (retained != source_reject::none) return reject(retained);

                // End any encoder created before the source became watched. Calling
                // CurrentCommandEncoder here would recreate state, so it never is.
                observation.stage = 14;
                metal->EndCurrentCommandEncoder();
                observation.stage = 15;
                observation.command_after_end = address_of(metal->CurrentCommandBuffer());
                observation.queue_after_end = address_of(cb.commandQueue);
                observation.status_after_end = uint64_t(cb.status);
                observation.properties_read |= 1024;
                if (observation.command_after_end != observation.command) return reject(source_reject::changed_command_after_end);
                if (observation.queue_after_end != observation.queue) return reject(source_reject::queue_after_end);
                if (observation.status_after_end > MTLCommandBufferStatusEnqueued) return reject(source_reject::submitted_after_end);

                observe_geometry(s);
                std::lock_guard<std::mutex> lock(s->model_mutex);
                const bool accepted = s->model.eof(buffer_entry->epoch.load(), texture_entry->identity, marker);
                return s->model.fault != fault_kind::none ? -204 : accepted ? 0 : 1;
            } @catch (NSException*) {
                return reject(source_reject::exception);
            }
        }
    }

    int failure(uint64_t session) {
        auto s = current();
        if (!s || s->model.session != session || s->stopping.load()) return 0;

        std::lock_guard<std::mutex> lock(s->model_mutex);
        return s->model.fault == fault_kind::none ? 0 : -204;
    }

    bool discard_owned_unsubmitted_copy(id<MTLCommandBuffer> command) {
        if (!command) return true;
        if (command.status != MTLCommandBufferStatusNotEnqueued) return false;
        auto buffer_entry = hook_of(command);
        if (!buffer_entry) return true;
        if (buffer_entry->kind != buffer_hook) return false;

        auto s = buffer_entry->state;
        std::lock_guard<std::mutex> lock(s->model_mutex);
        const uint64_t submission_id = buffer_entry->epoch.load();
        auto* record = s->model.buffer(submission_id);

        if (!record) {
            for (const auto& lease : s->leases) {
                if (lease.command == command) return false;
            }
            return true;
        }

        for (const auto& lease : s->leases) {
            if (lease.submission_id == submission_id && lease.command != command) {
                s->model.fail(fault_kind::identity);
                return false;
            }
        }

        if (!s->model.discard_unsubmitted_copy(submission_id, buffer_entry->identity)) return false;
        for (auto& lease : s->leases) {
            if (lease.submission_id == submission_id) {
                lease = gpu_lease{};
                break;
            }
        }

        buffer_entry->epoch.store(0);
        return true;
    }

    bool available() {
        auto s = current();
        if (!s || s->stopping.load()) return false;

        std::lock_guard<std::mutex> lock(s->model_mutex);
        return s->queue_address.load() && s->model.fault == fault_kind::none && s->model.counters.published > 0;
    }

    bool poll(presented_frame& result) {
        auto s = current();
        if (!s) return false;

        std::lock_guard<std::mutex> lock(s->model_mutex);
        return s->model.poll(result);
    }

    void stop(uint64_t session) {
        auto s = current();
        if (!s || s->model.session != session) return;

        s->stopping.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(s->model_mutex);
        s->model.stop();
    }

    int remove(uint64_t session) {
        if (!pthread_main_np()) return -202;
        auto s = current();
        if (!s) return 0;
        if (s->model.session != session) return -201;

        stop(session);
        {
            std::lock_guard<std::mutex> lock(s->model_mutex);
            if (!s->model.gpu_idle()) return 1;
        }

        std::lock_guard<std::mutex> lock(s->hook_mutex);
        bool conflict = false;

        for (auto& slot : s->hooks) {
            SmfPresentHookBox* box = slot.box;
            if (!box) continue;
            auto h = box->entry;
            id object = h->object;
            if (!object || !h->subclass) continue;
            Class actual = object_getClass(object);
            if (actual == h->subclass) {
                object_setClass(object, h->original);
            } else if (actual != h->original) {
                conflict = true;
            }
        }

        if (conflict) {
            std::lock_guard<std::mutex> model_lock(s->model_mutex);
            ++s->class_restore_conflicts;
            return 1;
        }
        if (s->calls.load(std::memory_order_acquire)) return 1;

        // Late dispatched trampolines stay protected by their associated box;
        // dropping these strong roots avoids queue and layer association cycles.
        {
            std::lock_guard<std::mutex> model_lock(s->model_mutex);
            s->queue = nil;
        }
        s->layer = nil;
        s->removed.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> current_lock(current_mutex);
        if (active == s) active.reset();
        return 0;
    }

    int snapshot(uint64_t session, present_status& result) {
        auto s = current();
        if (!s || s->model.session != session) return 1;

        std::lock_guard<std::mutex> lock(s->model_mutex);
        result = {};
        result.session = session;
        result.queue = s->queue_address.load();
        result.layer = s->layer_address;
        result.fault = uint32_t(s->model.fault);
        result.stopping = s->stopping.load();
        result.buffers = uint32_t(s->model.live_buffers());
        result.drawables = uint32_t(s->model.live_drawables());
        result.hooks = s->live_hooks.load();
        result.in_flight = s->calls.load();
        result.counters = s->model.counters;
        result.source_failures = s->source_failures;
        result.class_restore_conflicts = s->class_restore_conflicts;
        result.source_thread = s->source_thread;

        return 0;
    }

    int first_source_rejection(uint64_t session, source_rejection& result) {
        auto s = current();
        if (!s || s->model.session != session) return 1;

        std::lock_guard<std::mutex> lock(s->model_mutex);
        if (!s->first_rejection.reason) return 1;
        result = s->first_rejection;
        return 0;
    }

}
