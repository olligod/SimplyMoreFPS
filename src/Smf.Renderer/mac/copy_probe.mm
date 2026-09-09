#include "metal_owner.h"
#include "copy_probe.h"
#include <array>
#include <cstring>
#include <limits>

namespace mac {
    namespace {

        constexpr uint32_t capture_count = 4;
        constexpr uint32_t control_buffer_bytes = 4096;

        struct copy_completion {
            uint64_t command = 0, queue = 0, thread = 0, ns = 0;
            int64_t error = 0;
            uint32_t status = 0;
            std::atomic<bool> ready{false};
        };

        struct capture {
            mac_copy_probe_record record{};
            __strong id<MTLTexture> source = nil, owned = nil;
            __strong id<MTLBuffer> direct = nil, via = nil, control_source = nil, control = nil;
            __strong id<MTLCommandBuffer> command = nil;
            __strong id<MTLCommandQueue> queue = nil;
            copy_completion complete;
        };

        // Main and render access hold the session gate. A completion touches only its
        // own facts and then publishes ready; it never locks or edits the record.
        // Slots and their strong Metal objects survive process teardown, so no reuse
        // can race a late GPU callback, even after a rejected copy or a game quit.
        std::array<capture, capture_count>& captures = *new std::array<capture, capture_count>();
        uint64_t probe_session = 0;

        template<class T> uint64_t identity(T value) { return (uint64_t)(__bridge void*)value; }

        capture* find_capture(uint64_t session, uint64_t capture_id) {
            if (session != probe_session || capture_id < 1 || capture_id > capture_count) return nullptr;
            auto& c = captures[capture_id - 1];
            return c.record.request.capture_id == capture_id ? &c : nullptr;
        }

        bool same_target(const mac_target_probe_request& a, const mac_target_probe_request& b) {
            return std::memcmp(&a, &b, sizeof(a)) == 0;
        }

        void reject(capture& c, uint32_t reason, bool may_have_gpu = false) {
            c.record.reason = reason;
            c.record.state = may_have_gpu ? 7u : 6u;
        }

        bool completion_settled(const capture& c) {
            if (!c.complete.ready.load(std::memory_order_acquire)) return false;
            return c.complete.command == c.record.command_buffer && c.complete.queue == c.record.command_queue &&
                (c.complete.status == MTLCommandBufferStatusCompleted || c.complete.status == MTLCommandBufferStatusError);
        }

        void snapshot(const capture& c, mac_copy_probe_record& value) {
            value = c.record;

            if (c.complete.ready.load(std::memory_order_acquire)) {
                value.completion_command_buffer = c.complete.command;
                value.completion_queue = c.complete.queue;
                value.completion_thread = c.complete.thread;
                value.completion_ns = c.complete.ns;
                value.command_status_complete = c.complete.status;
                value.native_error_code = c.complete.error;
                value.flags |= 32u;

                bool okay = completion_settled(c) && c.complete.status == MTLCommandBufferStatusCompleted && !c.complete.error &&
                    (value.flags & 31u) == 31u && value.reason == 0;
                value.state = okay ? 5u : 7u;
                if (!okay && !value.reason) value.reason = 7;
            }
        }

        void register_completion(capture& c) {
            capture* held = &c;

            [c.command addCompletedHandler:^(id<MTLCommandBuffer> actual) {
                @autoreleasepool {
                    auto& done = held->complete;
                    done.command = identity(actual);
                    done.queue = identity(actual.commandQueue);
                    done.thread = native_thread();
                    done.ns = native_now();
                    done.error = actual.error.code;
                    done.status = uint32_t(actual.status);
                    done.ready.store(true, std::memory_order_release);
                }
            }];

            c.record.flags |= 16u;
        }

    }

    int enable_copy_probe(uint64_t session) {
        if (!session) return E_INVALIDARG;
        if (probe_session) return probe_session == session ? 0 : E_INVALIDARG;

        probe_session = session;
        return 0;
    }

    int stage_copy_probe(const mac_copy_probe_request& request) {
        const auto& t = request.target;

        if (t.size != 48 || t.version != 1 || t.session != probe_session || !t.source_frame || !t.native_render_buffer ||
            t.phase < 1 || t.phase > 2 || t.reserved || !t.width || !t.height || t.width > 4096 || t.height > 4096 ||
            uint64_t(t.width) * t.height * 4 > 8ull * 1024 * 1024 || request.capture_id < 1 || request.capture_id > capture_count ||
            request.pattern < 1 || request.pattern > 2 || request.reserved || !request.marker_width || !request.marker_height ||
            uint64_t(request.marker_x) + request.marker_width > t.width || uint64_t(request.marker_y) + request.marker_height > t.height) return E_INVALIDARG;

        auto& c = captures[request.capture_id - 1];
        if (c.record.request.capture_id) return c.record.state == 1 && std::memcmp(&c.record.request, &request, sizeof(request)) == 0 ? 0 : E_INVALIDARG;

        c.record.size = sizeof(c.record);
        c.record.version = 1;
        c.record.request = request;
        c.record.state = 1;
        c.record.command_status_at_encode = c.record.command_status_complete = c.record.store_before = c.record.store_after = UINT32_MAX;
        return 0;
    }

    int bind_copy_probe(const mac_target_probe_request& target) {
        for (uint32_t i = 0; i < capture_count; ++i) {
            auto& c = captures[i];
            if (c.record.state == 1 && same_target(c.record.request.target, target)) {
                c.record.state = 2;
                return int(i + 1);
            }
        }
        return 0;
    }

    void cancel_copy_probe(int index) {
        if (index < 1 || index > int(capture_count)) return;
        auto& c = captures[index - 1];
        if (c.record.state == 1 || c.record.state == 2) reject(c, 9);
    }

    void retire_copy_probe_stages() {
        for (auto& c : captures) {
            if (c.record.state == 1) reject(c, 9);
        }
    }

    bool copy_probe_in_flight() {
        for (auto& c : captures) {
            uint32_t s = c.record.state;
            if ((s == 2 || s == 3 || s == 4 || (s == 7 && (c.record.flags & 4))) && !completion_settled(c)) return true;
        }
        return false;
    }

    void execute_copy_probe(int index, IUnityGraphicsMetal* metal, const mac_target_probe_observation& target) {
        @autoreleasepool {
            if (index < 1 || index > int(capture_count)) return;

            auto& c = captures[index - 1];
            auto& r = c.record;

            if (r.state != 2 || !same_target(r.request.target, target.request)) {
                reject(c, 8);
                return;
            }

            r.target = target;
            r.begin_ns = native_now();
            r.state = 3;
            id<MTLBlitCommandEncoder> own = nil;

            @try {
                if (target.reason || !metal) {
                    reject(c, 1);
                    return;
                }

                id<MTLDevice> device = metal->MetalDevice();
                c.command = metal->CurrentCommandBuffer();
                c.queue = c.command.commandQueue;
                c.source = metal->TextureFromRenderBuffer((UnityRenderBuffer)(uintptr_t)r.request.target.native_render_buffer);
                MTLRenderPassDescriptor* pass = metal->CurrentRenderPassDescriptor();
                auto attachment = pass ? pass.colorAttachments[0] : nil;
                r.source_texture = identity(c.source);
                r.command_buffer = identity(c.command);
                r.command_queue = identity(c.queue);
                r.store_before = attachment ? uint32_t(attachment.storeAction) : UINT32_MAX;

                if (!device || !c.command || !c.queue || !pass || attachment.texture != c.source ||
                    identity(c.command) != target.command_after || identity(c.queue) != target.queue_after || identity(c.source) != target.texture ||
                    identity(device) != target.metal_device || c.command.device != device ||
                    !texture_valid(c.source, device, target.request.width, target.request.height) ||
                    (c.source.pixelFormat != MTLPixelFormatBGRA8Unorm && c.source.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) ||
                    c.command.status > MTLCommandBufferStatusEnqueued) {
                    reject(c, 2);
                    return;
                }

                r.flags |= 1u;
                r.row_bytes = (uint64_t(target.request.width) * 4 + 256) & ~uint64_t(255);
                r.direct_bytes = r.via_bytes = r.row_bytes * target.request.height;
                r.control_bytes = control_buffer_bytes;
                r.readback_bytes = r.direct_bytes + r.via_bytes + r.control_bytes;

                auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.source.pixelFormat width:target.request.width height:target.request.height mipmapped:NO];
                d.storageMode = MTLStorageModePrivate;
                d.usage = MTLTextureUsageShaderRead;
                c.owned = [device newTextureWithDescriptor:d];
                c.direct = [device newBufferWithLength:r.direct_bytes options:MTLResourceStorageModeShared];
                c.via = [device newBufferWithLength:r.via_bytes options:MTLResourceStorageModeShared];
                c.control_source = [device newBufferWithLength:control_buffer_bytes options:MTLResourceStorageModeShared];
                c.control = [device newBufferWithLength:control_buffer_bytes options:MTLResourceStorageModeShared];

                if (!c.owned || !c.direct || !c.via || !c.control_source || !c.control || !c.direct.contents || !c.via.contents || !c.control_source.contents || !c.control.contents ||
                    c.owned.device != device || c.direct.device != device || c.via.device != device || c.control_source.device != device || c.control.device != device) {
                    reject(c, 3);
                    return;
                }

                r.owned_texture = identity(c.owned);
                r.direct_buffer = identity(c.direct);
                r.via_buffer = identity(c.via);
                r.control_source_buffer = identity(c.control_source);
                r.control_buffer = identity(c.control);

                std::memset(c.direct.contents, 0xa5, size_t(r.direct_bytes));
                std::memset(c.via.contents, 0x5a, size_t(r.via_bytes));
                std::memset(c.control.contents, 0x3c, control_buffer_bytes);
                auto seed = static_cast<uint8_t*>(c.control_source.contents);
                for (uint32_t i = 0; i < control_buffer_bytes; ++i) seed[i] = uint8_t((i * 73 + r.request.capture_id * 29 + 11) & 255);

                // Public Unity contract: end its encoder, create and end ours on the
                // same uncommitted command buffer, and let Unity submit. There is
                // deliberately no CurrentCommandEncoder getter, new queue or wait.
                metal->EndCurrentCommandEncoder();
                r.end_unity_ns = native_now();
                r.flags |= 2u;
                id<MTLCommandBuffer> after = metal->CurrentCommandBuffer();
                r.store_after = attachment ? uint32_t(attachment.storeAction) : UINT32_MAX;
                r.command_status_at_encode = after ? uint32_t(after.status) : UINT32_MAX;

                if (after != c.command || after.commandQueue != c.queue || after.status > MTLCommandBufferStatusEnqueued) {
                    reject(c, 4);
                    return;
                }

                own = [c.command blitCommandEncoder];
                if (!own) {
                    reject(c, 5);
                    return;
                }

                r.flags |= 4u;
                auto origin = MTLOriginMake(0, 0, 0);
                auto extent = MTLSizeMake(target.request.width, target.request.height, 1);
                [own copyFromTexture:c.source sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:extent toBuffer:c.direct destinationOffset:0 destinationBytesPerRow:r.row_bytes destinationBytesPerImage:r.direct_bytes];
                [own copyFromTexture:c.source sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:extent toTexture:c.owned destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
                [own copyFromTexture:c.owned sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:extent toBuffer:c.via destinationOffset:0 destinationBytesPerRow:r.row_bytes destinationBytesPerImage:r.via_bytes];
                [own copyFromBuffer:c.control_source sourceOffset:0 toBuffer:c.control destinationOffset:0 size:control_buffer_bytes];

                [own endEncoding];
                own = nil;
                r.encoded_ns = native_now();
                r.flags |= 8u;
                r.state = 4;

                register_completion(c);
            } @catch (NSException* exception) {
                (void)exception;
                bool possible = (r.flags & 4) != 0;

                if (own) {
                    @try {
                        [own endEncoding];
                        r.flags |= 8u;
                        r.encoded_ns = native_now();
                    } @catch (NSException* ignored) {
                        (void)ignored;
                    }
                    own = nil;
                }

                reject(c, 6, possible);
                // A failed encode may still hold GPU work. Try to observe its
                // retirement; if registration also fails, quarantine until exit.
                if (possible && !(r.flags & 16)) {
                    @try {
                        register_completion(c);
                    } @catch (NSException* ignored) {
                        (void)ignored;
                    }
                }
            }
        }
    }

    int read_copy_probe(uint64_t session, uint64_t capture_id, mac_copy_probe_record& value) {
        auto c = find_capture(session, capture_id);
        if (!c) return 1;

        snapshot(*c, value);
        return 0;
    }

    int read_copy_bytes(uint64_t session, uint64_t capture_id, uint32_t route, void* output, uint32_t bytes) {
        auto c = find_capture(session, capture_id);
        if (!c || !output || route > 2) return E_INVALIDARG;

        mac_copy_probe_record record{};
        snapshot(*c, record);
        if (record.state != 5) return record.state == 6 || record.state == 7 ? E_FAIL : 1;

        id<MTLBuffer> buffer = route == 0 ? c->direct : route == 1 ? c->via : c->control;
        uint64_t wanted = route == 0 ? record.direct_bytes : route == 1 ? record.via_bytes : record.control_bytes;
        if (bytes != wanted || !buffer || buffer.length != wanted || !buffer.contents) return E_INVALIDARG;

        std::memcpy(output, buffer.contents, bytes);
        c->record.read_mask |= 1u << route;
        return 0;
    }

}
