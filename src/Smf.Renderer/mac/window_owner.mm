#include "metal_owner.h"
#include "geometry_diagnostics.h"
#include "display_geometry.h"

namespace mac {
    namespace {

        geometry_diagnostic first_geometry_failure;
        uint32_t geometry_failure_reason = 0;

        // Reasons: owner=1, bounds/scale=2, pixel extent=3, layout=4, color=5,
        // source device=6, source/display extent mismatch=7, overlay device=8.
        int geometry_reject(uint32_t reason, int result) {
            geometry_failure_reason = reason;
            return result;
        }

        void find_views(NSView* view, NSMutableArray<NSView*>* found) {
            if ([view.layer isKindOfClass:[CAMetalLayer class]]) [found addObject:view];
            for (NSView* child in view.subviews) find_views(child, found);
        }

        bool supported_color(CAMetalLayer* original) {
            return (original.pixelFormat == MTLPixelFormatBGRA8Unorm || original.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB) &&
                !original.wantsExtendedDynamicRangeContent;
        }

    }

    int window_owner::create(uint64_t window_address) {
        if (![NSThread isMainThread] || window || !window_address) return E_UNEXPECTED;

        first_geometry_failure = {};
        geometry_failure_reason = 0;
        image_width = image_height = 0;

        for (NSWindow* candidate in NSApp.windows) {
            if ((uint64_t)(__bridge void*)candidate == window_address) window = candidate;
        }

        if (!window || !window.contentView) {
            window = nil;
            return E_INVALIDARG;
        }

        auto found = [NSMutableArray<NSView*> array];
        find_views(window.contentView, found);
        if (found.count != 1) {
            window = nil;
            return E_NOTIMPL;
        }

        original_view = found[0];
        original = (CAMetalLayer*)original_view.layer;
        if (!supported_color(original)) {
            original_view = nil;
            original = nil;
            window = nil;
            return E_NOTIMPL;
        }

        number = (uint32_t)window.windowNumber;
        layer = [CAMetalLayer layer];
        layer.opaque = NO;
        layer.backgroundColor = nil;
        layer.opacity = 0;
        layer.pixelFormat = original.pixelFormat;
        layer.colorspace = original.colorspace;
        layer.wantsExtendedDynamicRangeContent = original.wantsExtendedDynamicRangeContent;
        layer.framebufferOnly = YES;
        layer.presentsWithTransaction = NO;
        layer.allowsNextDrawableTimeout = YES;
        // Compose at Unity's image resolution, then scale the finished image once,
        // as Unity does for a lower-resolution fullscreen window.
        layer.minificationFilter = kCAFilterLinear;
        layer.magnificationFilter = kCAFilterLinear;
        // Keep the original display pacing; there is no fixed camera FPS timer.
        layer.displaySyncEnabled = original.displaySyncEnabled;
        layer.maximumDrawableCount = 3;

        // Unity owns a layer-hosting view. AppKit forbids NSView children there, so
        // only our Core Animation sublayer is managed and Unity's view stays as is.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.frame = original.bounds;
        [original addSublayer:layer];
        [CATransaction commit];

        attached = true;
        int result = refresh_geometry();
        if (result < 0) {
            remove();
            return result;
        }

        result = drawable_observer::install(original, window);
        if (result < 0) {
            remove();
            return result;
        }

        return 0;
    }

    int window_owner::refresh_geometry() {
        if (![NSThread isMainThread] || !window || !layer || !attached || !original_view || layer.superlayer != original) return geometry_reject(1, E_UNEXPECTED);

        geometry_failure_reason = 0;
        NSRect bounds = original_view.bounds;
        double scale = window.backingScaleFactor;
        if (!std::isfinite(scale) || scale <= 0 || bounds.size.width <= 0 || bounds.size.height <= 0) return geometry_reject(2, E_INVALIDARG);

        const uint32_t w = (uint32_t)std::llround(bounds.size.width * scale);
        const uint32_t h = (uint32_t)std::llround(bounds.size.height * scale);
        if (!w || !h || w > 16384 || h > 16384) return geometry_reject(3, E_INVALIDARG);

        // Use Unity's real view, including one confined to a screen's safe area.
        // Arbitrary layer transforms still have no input mapping.
        NSRect original_rect = [original_view convertRect:original_view.bounds toView:window.contentView];
        if (!NSContainsRect(window.contentView.bounds, original_rect) || !CGRectEqualToRect(original.bounds, original_view.bounds) ||
            !CATransform3DIsIdentity(original.transform) || !CATransform3DIsIdentity(original.sublayerTransform) ||
            original.geometryFlipped || original.drawableSize.width != w || original.drawableSize.height != h) return geometry_reject(4, E_NOTIMPL);
        if (!supported_color(original) || layer.pixelFormat != original.pixelFormat) return geometry_reject(5, E_NOTIMPL);

        width = w;
        height = h;
        uint32_t iw = image_width ? image_width : w;
        uint32_t ih = image_height ? image_height : h;
        image_viewport viewport;
        if (!fit_image(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, iw, ih, viewport)) return geometry_reject(3, E_INVALIDARG);

        NSRect image_rect = NSMakeRect(viewport.x, viewport.y, viewport.width, viewport.height);
        bool color_changed = (layer.colorspace != original.colorspace) &&
            !(layer.colorspace && original.colorspace && CFEqual(layer.colorspace, original.colorspace));

        if (!CGRectEqualToRect(layer.frame, image_rect) || layer.contentsScale != scale || layer.drawableSize.width != iw ||
            layer.drawableSize.height != ih || color_changed || layer.displaySyncEnabled != original.displaySyncEnabled) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer.frame = image_rect;
            layer.contentsScale = scale;
            layer.drawableSize = CGSizeMake(iw, ih);
            layer.colorspace = original.colorspace;
            layer.displaySyncEnabled = original.displaySyncEnabled;
            [CATransaction commit];
        }

        NSRect screen = [window convertRectToScreen:[original_view convertRect:image_rect toView:nil]];
        double main_height = CGDisplayBounds(CGMainDisplayID()).size.height;
        camera_control::publish_geometry(screen.origin.x, main_height - NSMaxY(screen), screen.size.width, screen.size.height, viewport.scale_x, viewport.scale_y);
        return 0;
    }

    int window_owner::configure(id<MTLDevice> device, uint32_t w, uint32_t h) {
        if (![NSThread isMainThread]) return E_UNEXPECTED;

        auto reject = [&](uint32_t reason, int result) {
            if (first_geometry_failure.result >= 0) {
                geometry_diagnostic d;
                d.result = result;
                d.reason = reason;
                d.requested_width = w;
                d.requested_height = h;
                d.refreshed_width = width;
                d.refreshed_height = height;
                NSRect bounds = window.contentView.bounds;
                d.bounds_width = bounds.size.width;
                d.bounds_height = bounds.size.height;
                d.backing_scale = window.backingScaleFactor;
                d.drawable_width = original.drawableSize.width;
                d.drawable_height = original.drawableSize.height;
                d.source_device = (uint64_t)(__bridge void*)device;
                d.original_device = (uint64_t)(__bridge void*)original.device;
                d.flags = (attached ? 1u : 0u) | (visible ? 2u : 0u) | (window ? 4u : 0u) | (original ? 8u : 0u) | (layer ? 16u : 0u);

                retain_geometry_failure(first_geometry_failure, d);
            }

            return result;
        };

        if (!attached || !device || !original || original.device != device) return reject(6, E_UNEXPECTED);
        if (!w || !h || w > 16384 || h > 16384) return reject(3, E_INVALIDARG);

        image_width = w;
        image_height = h;
        int geometry = refresh_geometry();
        if (geometry < 0) return reject(geometry_failure_reason, geometry);

        if (layer.device && layer.device != device) return reject(8, E_NOTIMPL);
        if (!layer.device) layer.device = device;
        return 0;
    }

    uint64_t window_owner::show(bool shown) {
        if (![NSThread isMainThread] || !attached) return 0;

        uint64_t issued = ++transaction_issued;
        auto* owner = this;

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [CATransaction setCompletionBlock:^{
            uint64_t previous = owner->transaction_completed.load();
            while (previous < issued && !owner->transaction_completed.compare_exchange_weak(previous, issued, std::memory_order_release)) {
            }
        }];
        layer.opacity = shown ? 1 : 0;
        [CATransaction commit];

        visible = shown;
        drawable_observer::set_overlay_visible(shown);
        return issued;
    }

    int window_owner::remove() {
        if (![NSThread isMainThread] || visible) return E_UNEXPECTED;

        int removed = drawable_observer::remove();
        if (removed != 0) return removed;

        if (attached) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [layer removeFromSuperlayer];
            [CATransaction commit];
            attached = false;
        }

        layer = nil;
        original = nil;
        original_view = nil;
        window = nil;
        image_width = image_height = 0;

        return 0;
    }

}

SMF_MAC_API int32_t smf_mac_find_original_window(uint64_t* original_window) {
    if (!pthread_main_np() || !original_window) return E_INVALIDARG;

    *original_window = 0;
    for (NSWindow* candidate in NSApp.windows) {
        if (!candidate.contentView || !candidate.visible) continue;

        auto found = [NSMutableArray<NSView*> array];
        mac::find_views(candidate.contentView, found);
        if (found.count) {
            if (found.count != 1 || *original_window) return E_NOTIMPL;
            *original_window = (uint64_t)(__bridge void*)candidate;
        }
    }

    return *original_window ? 0 : 1;
}

// Diagnostics: cached scalar facts read on main; AppKit is never polled again.
SMF_MAC_API int32_t smf_mac_geometry_failure(mac::geometry_diagnostic* p, uint32_t bytes) {
    if (!pthread_main_np()) return E_UNEXPECTED;
    if (!p || bytes != sizeof(*p) || p->size != sizeof(*p) || p->version != 1) return E_INVALIDARG;

    *p = mac::first_geometry_failure;
    return p->result < 0 ? 0 : 1;
}
