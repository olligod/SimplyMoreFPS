#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <memory>
#include "../common/scene_snapshot.h"
#include "session_bridge.h"
#include "session_policy.h"
#define SMF_BRIDGE_INTERNAL
#include "camera_bridge.h"
#include "camera_control.h"

namespace session {

    class scene_channel;

    using Microsoft::WRL::ComPtr;

    constexpr size_t generation_count = 2;
    constexpr size_t ticket_count = 32;
    constexpr size_t operation_count = 16;
    constexpr size_t ack_count = 64;

    enum class ticket_kind { empty, pre_gui, frame, native_frame };

    struct ticket {
        ticket_kind kind = ticket_kind::empty;
        int32_t token = 0;
        bool canceled = false;
        bool consumed = false;
        session_pre_gui pre{};
        session_frame frame{};
        session_native_frame native{};
        // Main AddRefs these; the render thread releases them, also after a cancel.
        ID3D11Texture2D* first = nullptr;
        ID3D11Texture2D* second = nullptr;
        ID3D11Texture2D* cache = nullptr;
        smf_scene::snapshot scene{};
        bool has_scene = false;
    };

    struct operation {
        session_command command{};
        uint32_t phase = 0;
        uint64_t completion = 0;
        uint64_t frame = 0;
        bool used = false;
    };

    // Source visuals handed to the worker. The source keeps them alive until the worker
    // has processed their removal.
    struct link {
        uint64_t generation = 0;
        uint64_t content = 0;
        uint64_t serial = 0;
        uint64_t acknowledged = 0;
        IDCompositionVisual* background = nullptr;
        IDCompositionVisual* map = nullptr;
        IDCompositionVisual* hud = nullptr;
        bool attached = false;
    };

    struct affine {
        double a = 1;
        double b = 0;
        double c = 0;
        double d = 0;
        double e = 1;
        double f = 0;
    };

    // Camera pose the source frame was captured at, and the affine it projects with.
    struct model {
        affine nominal{};
        double x = 0;
        double z = 0;
        double projection_half_height = 0;
        uint64_t revision = 0;
        uint64_t camera_epoch = 0;
        uint64_t epoch = 0;
        int32_t map_id = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;
    };

    struct shared {
        SRWLOCK gate = SRWLOCK_INIT;
        session_status status{};
        std::array<ticket, ticket_count> tickets{};
        std::array<operation, operation_count> operations{};
        std::array<session_ack, ack_count> acks{};
        std::array<link, generation_count> links{};
        std::array<model, generation_count> models{};
        std::array<std::shared_ptr<scene_channel>, generation_count> scenes{};
        std::atomic<uint64_t> session{0};
        std::atomic<uint64_t> content{0};
        std::atomic<uint64_t> operation_fence{0};
        std::atomic<uint64_t> restore_after_frame{0};
        std::atomic<uint64_t> native_reject_through_frame{0};
        std::atomic<bool> captures_sealed{false};
        std::atomic<bool> stop_requested{false};
        std::atomic<uint32_t> main_thread{0};
        HWND window = nullptr;
        DWORD window_thread = 0;
        HANDLE worker = nullptr;
        HANDLE wake = nullptr;
        uint64_t next_ticket = 1;
        uint64_t next_link = 1;
        uint64_t ack_index = 0;
        uint64_t last_command = 0;
        uint64_t detach_requested = 0;
        uint64_t detach_completed = 0;
        bool ever_active = false;
        HRESULT stop_result = S_OK;
        uint64_t observer_epoch = 0;
    };

    // Leaked on purpose: no COM cleanup may run from CRT shutdown.
    shared& state();

    struct lock {
        SRWLOCK* gate;

        explicit lock(SRWLOCK& g) : gate(&g) {
            AcquireSRWLockExclusive(gate);
        }
        ~lock() {
            ReleaseSRWLockExclusive(gate);
        }
    };

    struct try_lock {
        SRWLOCK* gate;

        explicit try_lock(SRWLOCK& g) : gate(TryAcquireSRWLockExclusive(&g) ? &g : nullptr) {}
        ~try_lock() {
            if (gate) ReleaseSRWLockExclusive(gate);
        }

        explicit operator bool() const {
            return gate != nullptr;
        }
    };

    int64_t now();
    void wake();
    bool owned_window();
    void acknowledge(const session_command&, HRESULT, uint32_t evidence, uint64_t frame = 0,
        uint64_t commit = 0, bool superseded = false);
    bool is_current(uint64_t session, uint64_t content, uint64_t generation);

    // Render thread.
    void source_pump();
    void source_callback(ticket&);

    // Compositor worker thread.
    DWORD WINAPI worker_run(void*);
    HRESULT worker_poll_joined();

    // Commit completion thread. It only calls the thread-safe WaitForCommitCompletion
    // and reports back exactly the serial that was requested. At most four requests are held.
    HRESULT completion_start();
    uint64_t completion_request(IDCompositionDevice*, uint64_t serial, uint64_t session);
    HRESULT completion_poll(uint64_t token, uint64_t* completed = nullptr);
    bool completion_idle();
    void completion_request_stop();
    HRESULT completion_poll_joined();

    DXGI_FORMAT compatible_format(DXGI_FORMAT);
    bool projection(const session_pose&, uint32_t width, uint32_t height, affine&);
    bool root_projection(const model&, double x, double z, double projection_half_height, affine&);
    bool mapping(const affine& reference, affine source, uint32_t height, bool flip, D2D_MATRIX_3X2_F&);
    bool source_model(const model&, const session_pose&, const affine&, affine&);

}
