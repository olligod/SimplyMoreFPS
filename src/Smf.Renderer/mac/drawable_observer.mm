#include "drawable_observer.h"
#import <objc/runtime.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <cmath>
#include <mach/mach_time.h>
#include <pthread.h>

@interface SmfOriginalObserverBox : NSObject {
@public
    void* state;
}
@end
@implementation SmfOriginalObserverBox
@end

namespace drawable_observer {
    namespace {

        struct drawable_record {
            __weak id<CAMetalDrawable> drawable = nil;
            uint64_t sequence = 0, thread = 0, identity = 0, acquired_ns = 0;
            marker mark{};
            bool marked = false, overlay = false;
            uint64_t layer_identity = 0, texture_identity = 0, texture_device_identity = 0;
            uint32_t width = 0, height = 0, pixel_format = 0, texture_flags = 0;
            std::atomic<bool> done{false};
            double presented_time = 0;
            uint64_t presented_ns = 0;
        };

        // The state and the hook class live until process teardown. The strong AppKit
        // references are released on main after the original class is restored.
        struct observer_state {
            __strong CAMetalLayer* layer = nil;
            __strong NSWindow* window = nil;
            Class original = Nil, hook = Nil;
            IMP next = nullptr;
            bool original_framebuffer_only = true, class_restored = false;
            std::atomic<bool> stopping{false}, overlay{false};
            std::atomic<uint64_t> in_flight{0}, next_calls{0}, observed{0}, marked{0}, presented{0}, dropped{0}, overflow{0}, behind{0};
            std::atomic<uint64_t> latest_acquisition{0};
            std::mutex mutex;
            std::array<std::shared_ptr<drawable_record>, 8> records;
            std::shared_ptr<drawable_record> current;
            uint64_t delivered = 0;
            mac_selection_diagnostic last_arm{};
            uint64_t arm_calls = 0, original_layer_identity = 0, original_device_identity = 0;
            bool geometry_flipped_at_install = false;
            std::atomic<uint64_t> selection_mutex_misses{0};
        };

        observer_state* current = nullptr; // owned by the main and render API; the atomic below publishes it
        std::atomic<observer_state*> published{nullptr};
        std::atomic<uint64_t> class_serial{0};
        char association_key;

        uint64_t current_thread() {
            uint64_t value = 0;
            pthread_threadid_np(nullptr, &value);
            return value;
        }

        uint64_t now_ns() {
            static const auto timebase = [] {
                mach_timebase_info_data_t value{};
                mach_timebase_info(&value);
                return value;
            }();
            return uint64_t((long double)mach_absolute_time() * timebase.numer / timebase.denom);
        }

        bool contains_layer(CALayer* root, CALayer* target) {
            if (root == target) return true;
            for (CALayer* child in root.sublayers) {
                if (contains_layer(child, target)) return true;
            }
            return false;
        }

        bool contains_view(NSView* view, CALayer* target) {
            if (contains_layer(view.layer, target)) return true;
            for (NSView* child in view.subviews) {
                if (contains_view(child, target)) return true;
            }
            return false;
        }

        // Replacement IMP for nextDrawable on the observed layer instance.
        id<CAMetalDrawable> observed_next_drawable(id object, SEL selector) {
            SmfOriginalObserverBox* box = objc_getAssociatedObject(object, &association_key);
            observer_state* s = box ? static_cast<observer_state*>(box->state) : nullptr;

            // The association stays attached after the class is restored, so a method
            // dispatched before object_setClass can still call its original IMP.
            if (!s || !s->next) @throw [NSException exceptionWithName:NSInternalInconsistencyException reason:@"SMF original drawable dispatch lost its owner" userInfo:nil];

            s->in_flight.fetch_add(1, std::memory_order_acq_rel);

            @try {
                id<CAMetalDrawable> drawable = ((id<CAMetalDrawable> (*)(id, SEL))s->next)(object, selector);
                s->next_calls.fetch_add(1, std::memory_order_relaxed);
                uint64_t sequence = s->latest_acquisition.fetch_add(1, std::memory_order_acq_rel) + 1;

                if (s->stopping.load(std::memory_order_acquire) || !drawable) return drawable;
                if (![drawable respondsToSelector:@selector(addPresentedHandler:)] || ![drawable respondsToSelector:@selector(presentedTime)]) return drawable;

                auto record = std::make_shared<drawable_record>();
                record->drawable = drawable;
                record->thread = current_thread();
                record->acquired_ns = now_ns();
                record->identity = (uint64_t)(__bridge void*)drawable;

                // Cache the diagnostic metadata before handing this fresh drawable to
                // Unity. A presented drawable is never asked for its texture.
                id<MTLTexture> acquired_texture = drawable.texture;
                record->layer_identity = (uint64_t)(__bridge void*)drawable.layer;
                record->texture_identity = (uint64_t)(__bridge void*)acquired_texture;
                record->texture_device_identity = (uint64_t)(__bridge void*)acquired_texture.device;
                record->width = uint32_t(acquired_texture.width);
                record->height = uint32_t(acquired_texture.height);
                record->pixel_format = uint32_t(acquired_texture.pixelFormat);
                record->texture_flags = (record->layer_identity == s->original_layer_identity ? 16u : 0u) |
                    (acquired_texture && record->texture_device_identity == s->original_device_identity ? 32u : 0u) |
                    (acquired_texture.framebufferOnly ? 64u : 0u) | (acquired_texture ? 128u : 0u) | 256u;

                {
                    std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
                    if (!lock.owns_lock()) {
                        s->dropped.fetch_add(1);
                        return drawable;
                    }

                    size_t slot = s->records.size();
                    for (size_t i = 0; i < s->records.size(); ++i) {
                        if (!s->records[i]) {
                            slot = i;
                            break;
                        }
                    }

                    if (slot == s->records.size()) {
                        for (size_t i = 0; i < s->records.size(); ++i) {
                            if ((s->records[i]->done.load(std::memory_order_acquire) || s->records[i]->drawable == nil) &&
                                (slot == s->records.size() || s->records[i]->sequence < s->records[slot]->sequence)) slot = i;
                        }
                    }

                    if (slot == s->records.size()) {
                        s->current.reset();
                        s->overflow.fetch_add(1);
                        return drawable;
                    }

                    record->sequence = sequence;
                    s->records[slot] = record;
                    s->current = record;
                }

                [drawable addPresentedHandler:^(id<MTLDrawable> actual) {
                    // The record holds no strong drawable, so this block creates no
                    // retain cycle and does not exhaust the drawable pool.
                    double time = actual.presentedTime;
                    if ((uint64_t)(__bridge void*)actual == record->identity && std::isfinite(time) && time > 0) {
                        record->presented_time = time;
                        record->presented_ns = now_ns();
                        s->presented.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        s->dropped.fetch_add(1, std::memory_order_relaxed);
                    }

                    record->done.store(true, std::memory_order_release);
                }];

                s->observed.fetch_add(1, std::memory_order_relaxed);
                return drawable;
            } @finally {
                s->in_flight.fetch_sub(1, std::memory_order_release);
            }
        }

        // Caller holds s->mutex. Returns the current record only when it is armable.
        std::shared_ptr<drawable_record> current_locked(observer_state* s, uint32_t w, uint32_t h, mac_selection_diagnostic& trace) {
            auto record = s->current;
            trace = {};
            trace.operation = 2;
            trace.call_thread = current_thread();
            trace.checked_ns = now_ns();
            trace.requested_width = w;
            trace.requested_height = h;
            trace.latest_sequence = s->latest_acquisition.load(std::memory_order_acquire);

            // Weak availability and the metadata cached at acquisition come first; flag
            // 256 marks that timing. The lifetime and ownership checks follow.
            id<CAMetalDrawable> drawable = record ? record->drawable : nil;

            if (record) {
                trace.sequence = record->sequence;
                trace.record_thread = record->thread;
                trace.acquired_ns = record->acquired_ns;
                trace.flags = 1u | (record->done.load(std::memory_order_acquire) ? 2u : 0u) | (record->marked ? 4u : 0u) |
                    (drawable ? 8u : 0u) | record->texture_flags;
                trace.drawable = (uint64_t)(__bridge void*)drawable;
                trace.drawable_layer = record->layer_identity;
                trace.texture = record->texture_identity;
                trace.texture_device = record->texture_device_identity;
                trace.width = record->width;
                trace.height = record->height;
                trace.pixel_format = record->pixel_format;
            }

            if (s->stopping.load(std::memory_order_acquire)) {
                trace.reason = 2;
            } else if (!record) {
                trace.reason = 3;
            } else if (record->sequence != trace.latest_sequence) {
                trace.reason = 4;
            } else if (record->thread != trace.call_thread) {
                trace.reason = 5;
            } else if (record->done.load(std::memory_order_acquire)) {
                trace.reason = 6;
            } else if (!drawable) {
                trace.reason = 7;
            } else if (drawable.layer != s->layer) {
                trace.reason = 8;
            } else {
                id<MTLTexture> texture = drawable.texture;
                if (!texture) {
                    trace.reason = 9;
                } else if (texture.width != w || texture.height != h) {
                    trace.reason = 10;
                } else if (texture.device != s->layer.device) {
                    trace.reason = 11;
                }
            }

            return trace.reason ? std::shared_ptr<drawable_record>{} : record;
        }

    }

    int install(CAMetalLayer* original, NSWindow* window) {
        if (![NSThread isMainThread] || current || !original || !window || !window.contentView || !contains_view(window.contentView, original)) return -1;
        if (![original isKindOfClass:[CAMetalLayer class]]) return -2;

        SmfOriginalObserverBox* old = objc_getAssociatedObject(original, &association_key);
        if (old) {
            observer_state* previous = static_cast<observer_state*>(old->state);
            if (!previous || !previous->stopping.load(std::memory_order_acquire) || !previous->class_restored || previous->in_flight.load()) return -2;
        }

        Class original_class = object_getClass(original);
        SEL selector = @selector(nextDrawable);
        Method method = class_getInstanceMethod(original_class, selector);
        if (!method || method_getNumberOfArguments(method) != 2) return -3;

        NSString* name = [NSString stringWithFormat:@"SmfObservedMetalLayer_%llu", (unsigned long long)++class_serial];
        Class hook = objc_allocateClassPair(original_class, name.UTF8String, 0);
        if (!hook) return -4;
        if (!class_addMethod(hook, selector, (IMP)observed_next_drawable, method_getTypeEncoding(method)) || class_getInstanceSize(hook) != class_getInstanceSize(original_class)) {
            objc_disposeClassPair(hook);
            return -5;
        }
        objc_registerClassPair(hook);

        auto* s = new observer_state();
        s->layer = original;
        s->window = window;
        s->original = original_class;
        s->hook = hook;
        s->next = method_getImplementation(method);
        s->original_layer_identity = (uint64_t)(__bridge void*)original;
        s->original_device_identity = (uint64_t)(__bridge void*)original.device;
        s->geometry_flipped_at_install = original.geometryFlipped;
        s->original_framebuffer_only = original.framebufferOnly;

        SmfOriginalObserverBox* box = [SmfOriginalObserverBox new];
        box->state = s;
        objc_setAssociatedObject(original, &association_key, box, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        object_setClass(original, hook);
        current = s;
        published.store(s, std::memory_order_release);
        return 0;
    }

    int remove() {
        if (![NSThread isMainThread]) return -1;

        observer_state* s = current;
        if (!s) return 0;

        s->stopping.store(true, std::memory_order_release);
        if (!s->class_restored) {
            if (object_getClass(s->layer) != s->hook) return -2; // another owner changed it
            object_setClass(s->layer, s->original);
            s->class_restored = true;
        }

        if (s->in_flight.load(std::memory_order_acquire)) return 1;
        std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return 1;

        // Records and state stay for any late native completion. They hold only
        // weak drawables; removing the observer says nothing about GPU retirement.
        published.store(nullptr, std::memory_order_release);
        current = nullptr;
        s->layer = nil;
        s->window = nil;
        return 0;
    }

    void set_overlay_visible(bool value) {
        if ([NSThread isMainThread] && current) current->overlay.store(value, std::memory_order_release);
    }

    bool available() {
        observer_state* s = published.load(std::memory_order_acquire);
        return s && !s->stopping.load(std::memory_order_acquire) && s->observed.load(std::memory_order_acquire) > 0;
    }

    bool arm(const marker& value) {
        observer_state* s = published.load(std::memory_order_acquire);
        if (!s || s->stopping.load(std::memory_order_acquire) || !value.session || !value.frame || !value.width || !value.height) return false;

        std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            s->selection_mutex_misses.fetch_add(1);
            return false;
        }

        ++s->arm_calls;
        auto record = current_locked(s, value.width, value.height, s->last_arm);
        if (!record) return false;
        if (record->marked) {
            s->last_arm.reason = 13;
            return false;
        }

        record->mark = value;
        record->marked = true;
        record->overlay = s->overlay.load(std::memory_order_acquire);
        s->marked.fetch_add(1);
        return true;
    }

    bool poll(presented_frame& value) {
        value = {};
        observer_state* s = published.load(std::memory_order_acquire);
        if (!s) return false;

        std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;

        std::shared_ptr<drawable_record> best;
        for (auto& r : s->records) {
            if (r && r->marked && r->sequence > s->delivered && r->done.load(std::memory_order_acquire) && r->presented_time > 0 && (!best || best->sequence < r->sequence)) best = r;
        }
        if (!best) return false;

        static_cast<marker&>(value) = best->mark;
        value.serial = best->sequence;
        value.drawable = best->identity;
        value.presented_time = best->presented_time;
        value.acquired_ns = best->acquired_ns;
        value.presented_ns = best->presented_ns;
        value.overlay_visible = best->overlay;
        s->delivered = best->sequence;
        if (best->overlay) s->behind.fetch_add(1);

        return true;
    }

    status snapshot() {
        status value{};
        observer_state* s = published.load(std::memory_order_acquire);
        if (!s) return value;

        value.next_calls = s->next_calls.load();
        value.observed = s->observed.load();
        value.marked = s->marked.load();
        value.presented = s->presented.load();
        value.dropped = s->dropped.load();
        value.overflow = s->overflow.load();
        value.presented_behind_overlay = s->behind.load();
        value.in_flight = s->in_flight.load();
        value.installed = true;
        value.stopping = s->stopping.load();
        value.last_sequence = s->latest_acquisition.load(std::memory_order_acquire);

        return value;
    }

    int read_diagnostic(mac_observer_diagnostic& value) {
        value = {};
        value.size = sizeof(value);
        value.version = 1;

        observer_state* s = published.load(std::memory_order_acquire);
        if (!s) return 0;

        std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return 1;

        value.next_calls = s->next_calls.load();
        value.observed = s->observed.load();
        value.marked = s->marked.load();
        value.presented = s->presented.load();
        value.dropped = s->dropped.load();
        value.overflow = s->overflow.load();
        value.latest_sequence = s->latest_acquisition.load();
        value.in_flight = s->in_flight.load();
        value.original_layer = s->original_layer_identity;
        value.original_device = s->original_device_identity;
        value.arm_calls = s->arm_calls;
        value.selection_mutex_misses = s->selection_mutex_misses.load();
        value.flags = 1u | (s->stopping.load() ? 2u : 0u) | (s->geometry_flipped_at_install ? 8u : 0u) |
            (s->original_framebuffer_only ? 16u : 0u) | (s->current ? 32u : 0u);
        value.arm = s->last_arm;

        return 0;
    }

}
