#define SMF_PRESENT_OBSERVER_BUILD
#include "present_observer.h"
#include "present_format.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <cstring>
#include <utility>
#include "../vendor/minhook/include/MinHook.h"

using Microsoft::WRL::ComPtr;

namespace {

    constexpr uint32_t version = 1;
    constexpr uint32_t ring_size = 512;

    using present_fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using present1_fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

    present_fn original_present = nullptr;
    present1_fn original_present1 = nullptr;
    void* present_address = nullptr;
    void* present1_address = nullptr;
    HMODULE self_module = nullptr;
    HMODULE dxgi_module = nullptr;
    HWND original_window = nullptr;
    std::atomic<uint32_t> state{0};
    std::atomic<uint32_t> worker_thread{0};
    std::atomic<int32_t> failure_stage{0};
    std::atomic<int32_t> failure_code{0};
    std::atomic<bool> active{false};
    std::atomic<uint64_t> context_attempt{0};
    std::atomic<uint64_t> frame_attempt{0};
    std::atomic<uint64_t> marker_serial{0};
    std::atomic<uint64_t> hooks_entered{0};
    std::atomic<uint64_t> nested_calls{0};
    std::atomic<uint64_t> foreign_calls{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> superseded{0};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> overwrites{0};
    std::atomic<uint64_t> submissions{0};
    std::atomic<uint64_t> lock_misses{0};
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint32_t> discovery_destroyed{0};
    std::atomic<uint32_t> class_removed{0};
    std::atomic<uint32_t> descriptor_failures{0};
    uint8_t present_before[16]{};
    uint8_t present1_before[16]{};
    uint8_t present_installed[16]{};
    uint8_t present1_installed[16]{};
    int64_t qpc_frequency = 0;
    SRWLOCK gate_lock = SRWLOCK_INIT;
    SRWLOCK ring_lock = SRWLOCK_INIT;
    present_context context{};
    present_chain_status chain_status{sizeof(present_chain_status), 1};
    uint64_t begun_frame = 0;
    uint64_t begun_attempt = 0;
    DWORD begun_thread = 0;
    bool rendered_this_frame = false;
    ComPtr<IUnknown> bound_swap_chain;
    present_event ring[ring_size]{};
    present_event latest_submission{};
    present_event_v2 detailed_ring[ring_size]{};
    thread_local uint32_t depth = 0;
    thread_local uint32_t nested_in_outer = 0;

    int64_t now() {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    uint64_t identity(const void* value) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
    }

    struct try_lock {
        SRWLOCK* lock;

        explicit try_lock(SRWLOCK& value) : lock(TryAcquireSRWLockExclusive(&value) ? &value : nullptr) {
            if (!lock) lock_misses.fetch_add(1, std::memory_order_relaxed);
        }
        ~try_lock() {
            if (lock) ReleaseSRWLockExclusive(lock);
        }

        explicit operator bool() const {
            return lock != nullptr;
        }
    };

    // A render marker waiting for its Present.
    struct pending_marker {
        present_marker marker{};
        uint64_t serial = 0;
        uint64_t frame_attempt = 0;
        uint64_t context_attempt = 0;
        int64_t qpc = 0;
        DWORD thread = 0;
        bool unity_target = false;
        D3D11_TEXTURE2D_DESC desc{};
        ComPtr<ID3D11Texture2D> buffer;
        ComPtr<IUnknown> buffer_identity;
        ComPtr<IUnknown> device_identity;
    };

    pending_marker pending;

    void expire() {
        if (pending.serial) superseded.fetch_add(1, std::memory_order_relaxed);
        pending = pending_marker{};
    }

    bool same_context(const present_context& a, const present_context& b) {
        return a.session == b.session && a.epoch == b.epoch && a.restore_serial == b.restore_serial &&
            a.after_source_frame == b.after_source_frame && a.content_generation == b.content_generation &&
            a.resource_generation == b.resource_generation;
    }

    bool marker_matches(const present_marker& m) {
        return m.session == context.session && m.epoch == context.epoch &&
            m.restore_serial == context.restore_serial && m.content_generation == context.content_generation &&
            m.resource_generation == context.resource_generation && m.source_frame > context.after_source_frame;
    }

    void set_failure(int stage, int code) {
        active.store(false, std::memory_order_release);
        failure_stage.store(stage);
        failure_code.store(code);
        state.store(3, std::memory_order_release);
    }

    void store_event(present_event& event, const pending_marker& marker, uint32_t actual_quality) {
        try_lock held(ring_lock);
        if (!held) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The sequence is taken under the ring lock so a descheduled writer can never
        // overwrite a newer event in the same slot or move the latest submission back.
        event.sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;

        auto& slot = ring[(event.sequence - 1) % ring_size];
        if (slot.sequence) overwrites.fetch_add(1, std::memory_order_relaxed);
        slot = event;

        auto& detailed = detailed_ring[(event.sequence - 1) % ring_size];
        detailed = {};
        detailed.event = event;
        detailed.event.size = sizeof(detailed);
        detailed.event.version = 2;
        detailed.marker_width = marker.desc.Width;
        detailed.marker_height = marker.desc.Height;
        detailed.marker_format = static_cast<uint32_t>(marker.desc.Format);
        detailed.marker_sample_count = marker.desc.SampleDesc.Count;
        detailed.marker_sample_quality = marker.desc.SampleDesc.Quality;
        detailed.actual_sample_quality = actual_quality;

        produced.fetch_add(1, std::memory_order_relaxed);
        if (event.verdict == verdict_submission_only || event.verdict == verdict_unity_submission_only) {
            latest_submission = event;
            submissions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct observation {
        present_event event{};
        present_context captured_context{};
        pending_marker taken;
        uint64_t context_token = 0;
        uint64_t frame_token = 0;
        uint32_t actual_sample_quality = 0;
        bool record = false;
    };

    observation enter(IDXGISwapChain* swap, UINT interval, UINT flags, uint32_t api, int64_t entry) {
        observation out;
        if (state.load(std::memory_order_acquire) != 2 || !active.load(std::memory_order_acquire)) return out;

        DXGI_SWAP_CHAIN_DESC desc{};
        const HRESULT desc_hr = swap->GetDesc(&desc);
        if (FAILED(desc_hr)) {
            descriptor_failures.fetch_add(1, std::memory_order_relaxed);
            // Without the descriptor the swapchain identity is unknown, so expire the marker
            // rather than risk matching it against a later Present.
            frame_attempt.fetch_add(1, std::memory_order_acq_rel);
            return out;
        }

        if (desc.OutputWindow != original_window) {
            foreign_calls.fetch_add(1, std::memory_order_relaxed);
            return out;
        }

        out.record = true;
        auto& e = out.event;
        e.size = sizeof(e);
        e.version = version;
        e.api = api;
        e.thread_id = GetCurrentThreadId();
        e.sync_interval = interval;
        e.flags = flags;
        e.entry_qpc = entry;
        e.hwnd = reinterpret_cast<uintptr_t>(desc.OutputWindow);
        e.descriptor_width = desc.BufferDesc.Width;
        e.descriptor_height = desc.BufferDesc.Height;
        e.swap_effect = static_cast<uint32_t>(desc.SwapEffect);
        e.verdict = verdict_no_marker;

        ComPtr<IUnknown> swap_identity;
        HRESULT hr = swap->QueryInterface(IID_PPV_ARGS(&swap_identity));
        if (FAILED(hr)) {
            e.validation_result = hr;
            e.verdict = verdict_resource_failed;
            return out;
        }

        e.swap_chain_identity = identity(swap_identity.Get());

        {
            try_lock held(gate_lock);
            if (!held) {
                e.verdict = verdict_contended;
                return out;
            }

            out.captured_context = context;
            out.context_token = context_attempt.load(std::memory_order_acquire);
            out.frame_token = frame_attempt.load(std::memory_order_acquire);
            e.session = context.session;
            e.epoch = context.epoch;
            e.restore_serial = context.restore_serial;
            e.content_generation = context.content_generation;
            e.resource_generation = context.resource_generation;

            if (!pending.serial) return out;

            // Every top-level Present on the original window consumes the marker, even a
            // test or failed one, so a retried Present can never reuse it.
            out.taken = std::move(pending);
            pending = pending_marker{};
            e.marker_serial = out.taken.serial;
            e.source_frame = out.taken.marker.source_frame;
            e.marker_qpc = out.taken.qpc;
            e.marker_thread_id = out.taken.thread;
            e.marker_buffer_identity = identity(out.taken.buffer_identity.Get());
            e.marker_device_identity = identity(out.taken.device_identity.Get());

            if (out.taken.context_attempt != out.context_token || out.taken.frame_attempt != out.frame_token ||
                !active.load(std::memory_order_acquire)) {
                e.verdict = verdict_context_changed;
                return out;
            }

            if (bound_swap_chain && bound_swap_chain.Get() != swap_identity.Get()) {
                e.verdict = verdict_swap_chain_changed;
                return out;
            }
        }

        if (flags & DXGI_PRESENT_TEST) {
            e.verdict = verdict_test_present;
            return out;
        }

        if (e.marker_thread_id != e.thread_id) {
            e.verdict = verdict_thread_mismatch;
            return out;
        }

        if (e.marker_qpc > e.entry_qpc) {
            e.verdict = verdict_non_monotonic;
            return out;
        }

        ComPtr<ID3D11Texture2D> buffer;
        hr = swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) {
            e.validation_result = hr;
            e.verdict = verdict_resource_failed;
            return out;
        }

        ComPtr<IUnknown> resource_identity;
        ComPtr<IUnknown> device_identity;
        ComPtr<ID3D11Device> device;
        hr = buffer.As(&resource_identity);
        buffer->GetDevice(&device);
        if (SUCCEEDED(hr) && device) hr = device.As(&device_identity);
        if (FAILED(hr) || !device_identity) {
            e.validation_result = FAILED(hr) ? hr : E_NOINTERFACE;
            e.verdict = verdict_resource_failed;
            return out;
        }

        D3D11_TEXTURE2D_DESC actual{};
        buffer->GetDesc(&actual);
        e.buffer_identity = identity(resource_identity.Get());
        e.device_identity = identity(device_identity.Get());
        e.width = actual.Width;
        e.height = actual.Height;
        e.format = static_cast<uint32_t>(actual.Format);
        e.sample_count = actual.SampleDesc.Count;
        out.actual_sample_quality = actual.SampleDesc.Quality;

        if (!out.taken.unity_target && resource_identity.Get() != out.taken.buffer_identity.Get()) {
            e.verdict = verdict_resource_mismatch;
            return out;
        }

        if (device_identity.Get() != out.taken.device_identity.Get()) {
            e.verdict = verdict_device_mismatch;
            return out;
        }

        const auto& expected = out.taken.desc;
        if (!actual.Width || !actual.Height || actual.Width != expected.Width || actual.Height != expected.Height ||
            !present_format::matches(actual.Format, expected.Format, out.taken.unity_target) ||
            actual.SampleDesc.Count != expected.SampleDesc.Count ||
            actual.SampleDesc.Quality != expected.SampleDesc.Quality ||
            (desc.BufferDesc.Width && actual.Width != desc.BufferDesc.Width) ||
            (desc.BufferDesc.Height && actual.Height != desc.BufferDesc.Height)) {
            e.verdict = verdict_size_mismatch;
            return out;
        }

        {
            try_lock held(gate_lock);
            if (!held) {
                e.verdict = verdict_contended;
                return out;
            }

            if (context_attempt.load() != out.context_token || frame_attempt.load() != out.frame_token ||
                !same_context(context, out.captured_context) || !active.load()) {
                e.verdict = verdict_context_changed;
                return out;
            }

            // Only a real HWND plus render-buffer match binds the game's swapchain; the
            // discovery swapchain never gets here.
            if (!bound_swap_chain) {
                bound_swap_chain = swap_identity;
            } else if (bound_swap_chain.Get() != swap_identity.Get()) {
                e.verdict = verdict_swap_chain_changed;
                return out;
            }
        }

        // Tentative until finish() sees the actual S_OK.
        e.verdict = out.taken.unity_target ? verdict_unity_submission_only : verdict_submission_only;
        return out;
    }

    void finish(observation& out, HRESULT result, int64_t returned, uint32_t nested) {
        if (!out.record) return;

        auto& e = out.event;
        e.result = result;
        e.return_qpc = returned;
        e.nested_calls = nested;

        if (e.verdict == verdict_submission_only || e.verdict == verdict_unity_submission_only) {
            if (result != S_OK) {
                e.verdict = verdict_present_not_s_ok; // DXGI_STATUS_OCCLUDED is a success code but not S_OK
            } else if (context_attempt.load(std::memory_order_acquire) != out.context_token ||
                frame_attempt.load(std::memory_order_acquire) != out.frame_token || !active.load()) {
                e.verdict = verdict_context_changed;
            } else if (e.return_qpc < e.entry_qpc) {
                e.verdict = verdict_non_monotonic;
            }
        }

        store_event(e, out.taken, out.actual_sample_quality);
    }

    HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
        const DWORD incoming_error = GetLastError();
        hooks_entered.fetch_add(1, std::memory_order_relaxed);

        if (depth++) {
            ++nested_in_outer;
            nested_calls.fetch_add(1, std::memory_order_relaxed);
            SetLastError(incoming_error);
            const HRESULT result = original_present(swap, interval, flags);
            const DWORD outgoing_error = GetLastError();
            --depth;
            SetLastError(outgoing_error);
            return result;
        }

        nested_in_outer = 0;
        observation observed = enter(swap, interval, flags, 0, now());

        SetLastError(incoming_error);
        const HRESULT result = original_present(swap, interval, flags);
        const DWORD outgoing_error = GetLastError();
        const int64_t returned = now();
        finish(observed, result, returned, nested_in_outer);

        // Drop the marker's COM references before restoring the caller's LastError.
        observed.taken = pending_marker{};
        --depth;
        SetLastError(outgoing_error);
        return result;
    }

    HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* swap, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
        const DWORD incoming_error = GetLastError();
        hooks_entered.fetch_add(1, std::memory_order_relaxed);

        if (depth++) {
            ++nested_in_outer;
            nested_calls.fetch_add(1, std::memory_order_relaxed);
            SetLastError(incoming_error);
            const HRESULT result = original_present1(swap, interval, flags, parameters);
            const DWORD outgoing_error = GetLastError();
            --depth;
            SetLastError(outgoing_error);
            return result;
        }

        nested_in_outer = 0;
        observation observed = enter(swap, interval, flags, 1, now());

        SetLastError(incoming_error);
        // The parameters pointer is passed through untouched.
        const HRESULT result = original_present1(swap, interval, flags, parameters);
        const DWORD outgoing_error = GetLastError();
        const int64_t returned = now();
        finish(observed, result, returned, nested_in_outer);

        observed.taken = pending_marker{};
        --depth;
        SetLastError(outgoing_error);
        return result;
    }

    bool executable_bytes(void* address, uint8_t (&bytes)[16], MEMORY_BASIC_INFORMATION& info) {
        if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) return false;
        if (!(info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;

        SIZE_T read = 0;
        return ReadProcessMemory(GetCurrentProcess(), address, bytes, sizeof(bytes), &read) && read == sizeof(bytes);
    }

    // Decodes an unconditional jump at address: E9 rel32, EB rel8, FF 25 [rip+disp32], or mov rax/jmp rax.
    bool jump_target(void* address, const uint8_t (&bytes)[16], void*& target) {
        const auto base = reinterpret_cast<uintptr_t>(address);

        if (bytes[0] == 0xE9) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 1, 4);
            target = reinterpret_cast<void*>(base + 5 + static_cast<intptr_t>(displacement));
            return true;
        }

        if (bytes[0] == 0xEB) {
            target = reinterpret_cast<void*>(base + 2 + static_cast<int8_t>(bytes[1]));
            return true;
        }

        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 2, 4);
            SIZE_T read = 0;
            const auto slot = reinterpret_cast<void*>(base + 6 + static_cast<intptr_t>(displacement));
            return ReadProcessMemory(GetCurrentProcess(), slot, &target, sizeof(target), &read) && read == sizeof(target) && target;
        }

        if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
            std::memcpy(&target, bytes + 2, 8);
            return target != nullptr;
        }

        return false;
    }

    // Follows the detour chain at a dxgi.dll entry (an overlay may already be hooked in)
    // until it reaches code inside a loaded image. Anything undecodable fails closed.
    bool resolve_entry(void* address, uint8_t (&bytes)[16], uint64_t& relay, uint64_t& terminal, uint64_t& terminal_module,
        uint32_t& jumps, uint32_t& chained) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || module != dxgi_module) {
            return false;
        }

        MEMORY_BASIC_INFORMATION info{};
        if (!executable_bytes(address, bytes, info)) return false;

        uint8_t current[16]{};
        std::memcpy(current, bytes, sizeof(current));
        void* cursor = address;
        std::array<void*, 9> seen{};
        seen[0] = address;

        for (uint32_t hop = 0; hop < 9; ++hop) {
            void* next = nullptr;
            if (jump_target(cursor, current, next)) {
                if (hop == 8) return false;
                for (uint32_t i = 0; i <= hop; ++i) {
                    if (seen[i] == next) return false;
                }

                if (!hop) relay = identity(next);
                ++jumps;
                chained = 1;
                seen[hop + 1] = next;
                cursor = next;
                if (!executable_bytes(cursor, current, info)) return false;
                continue;
            }

            if (info.Type != MEM_IMAGE) return false;
            if ((current[0] == 0xFF && (current[1] == 0x25 || current[1] == 0x15)) || (current[0] == 0x48 && current[1] == 0xB8)) {
                return false;
            }

            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(cursor), &pinned)) {
                return false;
            }

            terminal = identity(cursor);
            terminal_module = identity(pinned);
            return true;
        }

        return false;
    }

    DWORD WINAPI install_worker(void*) {
        worker_thread.store(GetCurrentThreadId());

        const wchar_t* class_name = L"SMF.Renderer.PresentDiscovery";
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = self_module;
        wc.lpszClassName = class_name;
        const ATOM atom = RegisterClassW(&wc);

        if (!atom) {
            set_failure(1, static_cast<int>(GetLastError()));
            return 0;
        }

        HWND hidden = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"", WS_POPUP,
            -32000, -32000, 1, 1, nullptr, nullptr, self_module, nullptr);
        HRESULT hr = hidden ? S_OK : HRESULT_FROM_WIN32(GetLastError());

        ComPtr<IDXGISwapChain> swap;
        ComPtr<IDXGISwapChain1> swap1;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> immediate;

        if (hidden) {
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = 1;
            desc.BufferDesc.Height = 1;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.OutputWindow = hidden;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &immediate);
            if (SUCCEEDED(hr)) hr = swap.As(&swap1);

            if (SUCCEEDED(hr)) {
                present_address = (*reinterpret_cast<void***>(swap.Get()))[8];
                present1_address = (*reinterpret_cast<void***>(swap1.Get()))[22];
            }
        }

        // The discovery window is never shown or presented; everything is torn down before patching.
        swap1.Reset();
        swap.Reset();
        immediate.Reset();
        device.Reset();
        if (hidden) discovery_destroyed.store(DestroyWindow(hidden) ? 1u : 0u);
        class_removed.store(UnregisterClassW(class_name, self_module) ? 1u : 0u);

        if (FAILED(hr) || !discovery_destroyed.load() || !class_removed.load()) {
            set_failure(2, FAILED(hr) ? hr : E_FAIL);
            return 0;
        }

        dxgi_module = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi_module || present_address == present1_address ||
            !resolve_entry(present_address, present_before, chain_status.present_relay, chain_status.present_terminal,
                chain_status.present_terminal_module, chain_status.present_jumps, chain_status.present_chained) ||
            !resolve_entry(present1_address, present1_before, chain_status.present1_relay, chain_status.present1_terminal,
                chain_status.present1_terminal_module, chain_status.present1_jumps, chain_status.present1_chained)) {
            set_failure(3, E_NOINTERFACE);
            return 0;
        }

        HMODULE pinned_dxgi = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(present_address), &pinned_dxgi)) {
            set_failure(4, static_cast<int>(GetLastError()));
            return 0;
        }

        MH_STATUS mh = MH_Initialize();
        if (mh != MH_OK) {
            set_failure(5, mh);
            return 0;
        }

        mh = MH_CreateHook(present_address, reinterpret_cast<void*>(&hook_present), reinterpret_cast<void**>(&original_present));
        if (mh != MH_OK) {
            set_failure(6, mh);
            return 0;
        }

        mh = MH_CreateHook(present1_address, reinterpret_cast<void*>(&hook_present1), reinterpret_cast<void**>(&original_present1));
        if (mh != MH_OK) {
            set_failure(7, mh);
            return 0;
        }

        // MinHook forwards an existing detour unchanged. Refuse if either entry changed
        // under us during discovery.
        if (std::memcmp(present_address, present_before, 16) || std::memcmp(present1_address, present1_before, 16)) {
            set_failure(12, HRESULT_FROM_WIN32(ERROR_RETRY));
            return 0;
        }

        mh = MH_QueueEnableHook(present_address);
        if (mh == MH_OK) mh = MH_QueueEnableHook(present1_address);
        if (mh != MH_OK) {
            set_failure(8, mh);
            return 0;
        }

        // One batch for the process lifetime. Hooks are never toggled or removed later, so
        // no trampoline can be freed while another thread is still inside it.
        mh = MH_ApplyQueued();
        if (mh != MH_OK) {
            set_failure(9, mh);
            return 0;
        }

        std::memcpy(present_installed, present_address, 16);
        std::memcpy(present1_installed, present1_address, 16);
        state.store(2, std::memory_order_release);
        return 0;
    }

    int render_marker(const present_marker* value, void* actual_backbuffer, bool unity_target) {
        if (!value || value->size != sizeof(*value) || value->version != version || !actual_backbuffer || !active.load()) return 0;

        pending_marker next;
        next.marker = *value;
        next.qpc = now();
        next.thread = GetCurrentThreadId();
        next.unity_target = unity_target;

        // The caller is a native render callback and hands us a live D3D11 texture.
        next.buffer = static_cast<ID3D11Texture2D*>(actual_backbuffer);
        HRESULT hr = next.buffer.As(&next.buffer_identity);
        ComPtr<ID3D11Device> device;
        next.buffer->GetDevice(&device);
        if (SUCCEEDED(hr) && device) hr = device.As(&next.device_identity);

        if (FAILED(hr) || !next.device_identity) {
            rejected.fetch_add(1);
            return 0;
        }

        next.buffer->GetDesc(&next.desc);
        if (!next.desc.Width || !next.desc.Height) {
            rejected.fetch_add(1);
            return 0;
        }

        try_lock held(gate_lock);
        if (!held) return 0;

        if (!active.load() || !marker_matches(*value) || begun_frame != value->source_frame ||
            begun_thread != next.thread || begun_attempt != frame_attempt.load() || rendered_this_frame) {
            rejected.fetch_add(1);
            return 0;
        }

        next.context_attempt = context_attempt.load();
        next.frame_attempt = begun_attempt;
        next.serial = marker_serial.fetch_add(1) + 1;
        rendered_this_frame = true;
        pending = std::move(next);
        accepted.fetch_add(1);
        return 1;
    }

}

int smf_po_start(uint64_t hwnd) {
    DWORD pid = 0;
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
    if (!window || !IsWindow(window) || !GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId()) return 0;

    uint32_t expected = 0;
    if (!state.compare_exchange_strong(expected, 1)) {
        // original_window is only settled once the worker has finished (state 2 or 3).
        return (expected == 2 || expected == 3) && original_window == window ? 1 : 0;
    }

    original_window = window;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&smf_po_start), &self_module)) {
        set_failure(10, static_cast<int>(GetLastError()));
        return 0;
    }

    HANDLE worker = CreateThread(nullptr, 0, install_worker, nullptr, 0, nullptr);
    if (!worker) {
        set_failure(11, static_cast<int>(GetLastError()));
        return 0;
    }

    CloseHandle(worker); // nobody waits for it
    return 1;
}

int smf_po_context(const present_context* value) {
    if (!value || value->size != sizeof(*value) || value->version != version || state.load() != 2) return 0;

    active.store(false, std::memory_order_release);
    context_attempt.fetch_add(1, std::memory_order_acq_rel);

    try_lock held(gate_lock);
    if (!held) return 0;
    // Re-sending the same active context is rejected; the epoch has to increase.
    if (value->session && (!value->epoch || value->epoch <= context.epoch)) return 0;

    expire();
    bound_swap_chain.Reset();
    begun_frame = 0;
    begun_thread = 0;
    begun_attempt = 0;
    rendered_this_frame = false;

    const uint64_t previous_epoch = context.epoch;
    context = *value;
    if (!value->session) context.epoch = previous_epoch;
    active.store(value->session != 0, std::memory_order_release);
    return 1;
}

int smf_po_begin_source_frame(uint64_t session, uint64_t epoch, uint64_t source_frame) {
    const uint64_t attempt = frame_attempt.fetch_add(1, std::memory_order_acq_rel) + 1;

    try_lock held(gate_lock);
    if (!held) return 0;
    expire();
    rendered_this_frame = false;

    if (!active.load() || context.session != session || context.epoch != epoch ||
        source_frame <= begun_frame || source_frame <= context.after_source_frame) {
        rejected.fetch_add(1);
        return 0;
    }

    begun_frame = source_frame;
    begun_thread = GetCurrentThreadId();
    begun_attempt = attempt;
    return 1;
}

int smf_po_rendered(const present_marker* value, void* actual_backbuffer) {
    return render_marker(value, actual_backbuffer, false);
}

int smf_po_rendered_unity_target(const present_marker* value, void* completed_unity_target) {
    return render_marker(value, completed_unity_target, true);
}

int smf_po_status(present_status* value) {
    if (!value || value->size != sizeof(*value) || value->version != version) return 0;

    const uint32_t observed_state = state.load(std::memory_order_acquire);
    present_status s{};
    s.size = sizeof(s);
    s.version = version;
    s.state = observed_state;
    s.worker_thread = worker_thread.load();
    s.failure_stage = failure_stage.load();
    s.failure_code = failure_code.load();

    // The install fields are only stable once the worker has left state 1.
    if (observed_state == 2 || observed_state == 3) {
        s.hwnd = reinterpret_cast<uintptr_t>(original_window);
        s.present_entry = reinterpret_cast<uintptr_t>(present_address);
        s.present1_entry = reinterpret_cast<uintptr_t>(present1_address);
        s.dxgi_module = reinterpret_cast<uintptr_t>(dxgi_module);
        s.qpc_frequency = qpc_frequency;
        std::memcpy(s.present_before, present_before, 16);
        std::memcpy(s.present1_before, present1_before, 16);
        std::memcpy(s.present_installed, present_installed, 16);
        std::memcpy(s.present1_installed, present1_installed, 16);

        if (observed_state == 2) {
            s.installed_code_matches = std::memcmp(present_address, present_installed, 16) == 0 &&
                std::memcmp(present1_address, present1_installed, 16) == 0;
        }
    }

    {
        try_lock held(gate_lock);
        if (!held) return 0;
        s.session = context.session;
        s.epoch = context.epoch;
        s.restore_serial = context.restore_serial;
        s.last_begun_frame = begun_frame;
    }

    s.hooks_entered = hooks_entered.load();
    s.nested_calls = nested_calls.load();
    s.foreign_window_calls = foreign_calls.load();
    s.marker_accepted = accepted.load();
    s.marker_rejected = rejected.load();
    s.marker_superseded = superseded.load();
    s.events_produced = produced.load();
    s.events_dropped = dropped.load();
    s.ring_overwrites = overwrites.load();
    s.submissions = submissions.load();
    s.lock_misses = lock_misses.load();
    s.last_event_sequence = sequence.load();
    s.discovery_window_destroyed = discovery_destroyed.load();
    s.discovery_class_removed = class_removed.load();
    s.descriptor_failures = descriptor_failures.load();

    *value = s;
    return 1;
}

int smf_po_chain_status(present_chain_status* value) {
    if (!value || value->size != sizeof(*value) || value->version != version) return 0;
    const uint32_t observed = state.load(std::memory_order_acquire);
    if (observed != 2 && observed != 3) return 0;

    *value = chain_status;
    return 1;
}

int smf_po_read(uint64_t after_sequence, present_event* events, uint32_t capacity) {
    if (!events || !capacity || capacity > 64) return -1;

    try_lock held(ring_lock);
    if (!held) return -1;
    const uint64_t last = sequence.load();
    if (after_sequence >= last) return 0;
    uint64_t first = after_sequence + 1;
    if (last >= ring_size && first <= last - ring_size) first = last - ring_size + 1;

    uint32_t count = 0;
    for (uint64_t serial = first; serial <= last && count < capacity; ++serial) {
        const auto& event = ring[(serial - 1) % ring_size];
        if (event.sequence == serial) events[count++] = event;
    }

    return static_cast<int>(count);
}

int smf_po_read_v2(uint64_t after_sequence, present_event_v2* events, uint32_t capacity) {
    if (!events || !capacity || capacity > 64) return -1;

    try_lock held(ring_lock);
    if (!held) return -1;
    const uint64_t last = sequence.load();
    if (after_sequence >= last) return 0;
    uint64_t first = after_sequence + 1;
    if (last >= ring_size && first <= last - ring_size) first = last - ring_size + 1;

    uint32_t count = 0;
    for (uint64_t serial = first; serial <= last && count < capacity; ++serial) {
        const auto& event = detailed_ring[(serial - 1) % ring_size];
        if (event.event.sequence == serial) events[count++] = event;
    }

    return static_cast<int>(count);
}

int smf_po_last_submission(const present_context* expected, present_event* event) {
    if (!expected || expected->size != sizeof(*expected) || expected->version != version || !event) return 0;
    if (event->size != sizeof(*event) || event->version != version || !active.load()) return 0;

    const uint64_t token = context_attempt.load();
    {
        try_lock held(gate_lock);
        if (!held || !same_context(*expected, context)) return 0;
    }

    try_lock held(ring_lock);
    if (!held) return 0;
    const auto& s = latest_submission;
    if ((s.verdict != verdict_submission_only && s.verdict != verdict_unity_submission_only) ||
        s.session != expected->session || s.epoch != expected->epoch || s.restore_serial != expected->restore_serial ||
        s.source_frame <= expected->after_source_frame || s.content_generation != expected->content_generation ||
        s.resource_generation != expected->resource_generation || token != context_attempt.load() || !active.load()) {
        return 0;
    }

    *event = s;
    return 1;
}
