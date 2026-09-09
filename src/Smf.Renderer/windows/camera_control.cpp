#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SMF_BRIDGE_INTERNAL
#include "camera_control.h"
#include "wheel_queue.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace camera_control {
    namespace {

        constexpr uint32_t capacity = 128;
        static_assert(std::atomic<uint64_t>::is_always_lock_free, "x64 SPSC counters must be lock-free");

        SRWLOCK gate = SRWLOCK_INIT;
        smf_control_policy policy{};
        smf_control_impulse impulses[capacity]{};
        smf_control_policy worker_policy{}; // worker's copy of the last complete policy
        std::atomic<uint64_t> head{0};
        std::atomic<uint64_t> tail{0};
        std::atomic<uint64_t> consumed{0};
        std::atomic<uint64_t> stale{0};
        std::atomic<uint64_t> blocked_count{0};
        std::atomic<uint64_t> full{0};
        std::atomic<uint64_t> busy{0};
        uint32_t high_water = 0; // main thread only
        uint64_t last_sequence = 0;

        wheel_queue<1024> wheel_events;
        SRWLOCK wheel_lifetime = SRWLOCK_INIT;
        HANDLE wheel_thread = nullptr;
        HANDLE wheel_stop = nullptr;
        HWND wheel_window = nullptr;
        HHOOK wheel_hook = nullptr; // hook thread only
        std::atomic<uint32_t> wheel_state{0};
        std::atomic<uint32_t> wheel_thread_id{0};
        std::atomic<int32_t> wheel_error{0};
        std::atomic<uint64_t> wheel_generation{0};
        std::atomic<uint64_t> wheel_epoch{0};
        std::atomic<uint64_t> wheel_observed{0};
        std::atomic<uint64_t> wheel_consumed{0};
        std::atomic<uint64_t> wheel_denied{0};
        std::atomic<uint64_t> wheel_stale{0};
        std::atomic<uint64_t> wheel_overflow{0};
        std::atomic<bool> wheel_enabled{false};
        std::atomic<bool> wheel_fault{false};
        std::atomic<uint64_t> wheel_source{0};
        std::atomic<uint64_t> wheel_start_policy_revision{0}; // written under the policy gate
        std::atomic<uint32_t> wheel_reservations{0};

        struct try_lock {
            bool held = TryAcquireSRWLockExclusive(&gate) != FALSE;

            ~try_lock() {
                if (held) ReleaseSRWLockExclusive(&gate);
            }
        };

        bool finite(double n) {
            return std::isfinite(n);
        }

        bool key_down(int key) {
            return (GetAsyncKeyState(key) & 0x8000) != 0;
        }

        LRESULT CALLBACK observe_wheel(int code, WPARAM message, LPARAM data) {
            if (code == HC_ACTION && message == WM_MOUSEWHEEL && wheel_enabled.load(std::memory_order_acquire) && !wheel_fault.load()) {
                const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
                POINT point = mouse->pt;
                RECT rect{};
                HWND hit = WindowFromPoint(mouse->pt);
                const uint64_t epoch = wheel_epoch.load(std::memory_order_acquire);

                if (GetForegroundWindow() == wheel_window && hit && GetAncestor(hit, GA_ROOT) == wheel_window &&
                    ScreenToClient(wheel_window, &point) && GetClientRect(wheel_window, &rect) && PtInRect(&rect, point)) {
                    ++wheel_observed;

                    const uint32_t modifiers = wheel_modifiers(key_down(VK_CONTROL), key_down(VK_MENU), key_down(VK_SHIFT));
                    if (!epoch || !wheel_modifiers_allowed(wheel_reservations.load(), modifiers)) {
                        ++wheel_denied;
                    } else {
                        const wheel_event event{wheel_generation.load(), epoch, static_cast<short>(HIWORD(mouse->mouseData)),
                            point.x, point.y, modifiers};
                        if (!wheel_events.push(event)) {
                            ++wheel_overflow;
                            wheel_fault.store(true);
                            wheel_epoch.store(0);
                            wheel_error.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
                            wheel_state.store(4);
                        }
                    }
                }
            }

            return CallNextHookEx(wheel_hook, code, message, data);
        }

        DWORD WINAPI wheel_pump(void*) {
            MSG message{};
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&observe_wheel), &module);
            wheel_hook = module ? SetWindowsHookExW(WH_MOUSE_LL, observe_wheel, module, 0) : nullptr;
            wheel_thread_id.store(GetCurrentThreadId());

            if (!wheel_hook) {
                wheel_error.store(HRESULT_FROM_WIN32(GetLastError()));
                wheel_state.store(4);
                return 0;
            }

            wheel_state.store(2);
            for (;;) {
                const DWORD result = MsgWaitForMultipleObjectsEx(1, &wheel_stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (result == WAIT_OBJECT_0) break;
                if (result != WAIT_OBJECT_0 + 1) {
                    wheel_error.store(HRESULT_FROM_WIN32(GetLastError()));
                    break;
                }

                for (unsigned i = 0; i < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); i++) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }

            if (!UnhookWindowsHookEx(wheel_hook)) wheel_error.store(HRESULT_FROM_WIN32(GetLastError()));
            wheel_hook = nullptr;
            wheel_enabled.store(false);
            wheel_epoch.store(0);
            wheel_state.store(wheel_error.load() ? 4u : 3u);
            return 0;
        }

    }

    HRESULT publish_policy(const smf_control_policy& p) {
        if (p.size != sizeof(p) || p.version != 1 || !p.epoch || !p.revision || p.rect_count > 64 || (p.flags & ~127u)) {
            return E_INVALIDARG;
        }
        if (!finite(p.ui_scale) || p.ui_scale <= 0 || !finite(p.inspect_height) || p.inspect_height < 0) return E_INVALIDARG;

        for (uint32_t i = 0; i < p.rect_count; ++i) {
            const auto& r = p.rects[i];
            if (!finite(r.left) || !finite(r.top) || !finite(r.right) || !finite(r.bottom)) return E_INVALIDARG;
            if (r.right < r.left || r.bottom < r.top) return E_INVALIDARG;
        }

        try_lock lock;
        if (!lock.held) {
            ++busy;
            return S_FALSE;
        }

        if (policy.revision >= p.revision) return E_INVALIDARG;
        policy = p;
        wheel_reservations.store(p.flags);
        return S_OK;
    }

    HRESULT queue(const smf_control_impulse& p) {
        if (p.size != sizeof(p) || p.version != 1 || !p.epoch || !p.sequence || p.reserved || (p.flags & ~3u)) {
            return E_INVALIDARG;
        }
        if (!finite(p.wheel_delta) || (p.wheel_delta == 0 && !p.flags)) return E_INVALIDARG;
        // Wheel input comes from the hook only; a managed wheel impulse is never accepted.
        if (p.wheel_delta != 0) return E_INVALIDARG;
        if (p.sequence <= last_sequence) return E_INVALIDARG;

        const uint64_t next = head.load(std::memory_order_relaxed);
        const uint64_t first = tail.load(std::memory_order_acquire);
        if (next - first >= capacity) {
            ++full;
            return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
        }

        // One producer (main) and one consumer (worker); the entry is published by the head store.
        impulses[next % capacity] = p;
        last_sequence = p.sequence;
        high_water = std::max(high_water, static_cast<uint32_t>(next - first + 1));
        head.store(next + 1, std::memory_order_release);

        return S_OK;
    }

    HRESULT read_status(smf_control_status& result) {
        try_lock lock;
        if (!lock.held) return S_FALSE;

        const uint64_t next = head.load(std::memory_order_acquire);
        const uint64_t first = tail.load(std::memory_order_acquire);
        result = {sizeof(result), 1, static_cast<uint32_t>(next - first), high_water, next,
            consumed.load(), stale.load(), blocked_count.load(), full.load(), busy.load(), policy.revision};
        return S_OK;
    }

    void prepare(uint64_t epoch, bool blocked, double x, double y, smf_camera_input& input) {
        {
            try_lock lock;
            if (lock.held) {
                worker_policy = policy;
            } else {
                ++busy;
            }
        }

        if (worker_policy.epoch == epoch) {
            if ((worker_policy.flags & 1) != 0) input.flags |= SMF_CAMERA_ALLOW_EDGE_SCROLL;
            if ((worker_policy.flags & 2) != 0) input.flags |= SMF_CAMERA_FULLSCREEN;
            input.inspect_pane_height = worker_policy.inspect_height;

            for (uint32_t i = 0; i < worker_policy.rect_count; ++i) {
                const auto& r = worker_policy.rects[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
                    input.flags |= SMF_CAMERA_POINTER_OVER_UI;
                    break;
                }
            }
        }

        // Drain only what existed when this step started so new input cannot keep us looping.
        const uint64_t end = head.load(std::memory_order_acquire);
        uint64_t first = tail.load(std::memory_order_relaxed);

        while (first < end) {
            const smf_control_impulse p = impulses[first % capacity];
            tail.store(++first, std::memory_order_release);
            if (p.epoch != epoch) {
                ++stale;
                continue;
            }

            // A pulse queued before losing focus or opening a menu is dropped, not replayed.
            if (blocked) {
                ++blocked_count;
                continue;
            }

            input.wheel_delta = p.wheel_delta;
            if (p.flags & 1) input.flags |= SMF_CAMERA_ZOOM_IN_PULSE;
            if (p.flags & 2) input.flags |= SMF_CAMERA_ZOOM_OUT_PULSE;
            ++consumed;
            break;
        }

        const bool wheel_ready = wheel_enabled.load() && wheel_state.load() == 2 && !wheel_fault.load() &&
            worker_policy.revision > wheel_start_policy_revision && worker_policy.epoch == epoch &&
            (worker_policy.flags & 12u) == 8u && !blocked;
        wheel_epoch.store(wheel_ready ? epoch : 0, std::memory_order_release);

        // One impulse per step, key pulses first. Wheel events are never merged or cancelled.
        if (input.flags & (SMF_CAMERA_ZOOM_IN_PULSE | SMF_CAMERA_ZOOM_OUT_PULSE)) return;

        wheel_event event{};
        for (unsigned i = 0; i < 1024 && wheel_events.pop(event); i++) {
            if (!wheel_event_matches(event, wheel_generation.load(), epoch)) {
                ++wheel_stale;
                continue;
            }

            if (!wheel_ready || !wheel_modifiers_allowed(worker_policy.flags, event.modifiers) ||
                !wheel_policy_allows(worker_policy, epoch, blocked, event.x, event.y)) {
                ++wheel_denied;
                continue;
            }

            input.wheel_delta = wheel_unity_delta(event.delta);
            ++wheel_consumed;
            break;
        }
    }

    void clear() {
        const uint64_t end = head.load(std::memory_order_acquire);
        const uint64_t first = tail.load(std::memory_order_relaxed);
        stale.fetch_add(end - first);
        tail.store(end, std::memory_order_release);

        wheel_epoch.store(0, std::memory_order_release);
        wheel_event event{};
        for (unsigned i = 0; i < 1024 && wheel_events.pop(event); i++) {
            ++wheel_stale;
        }

        // Sequence and policy revision stay cumulative across sessions.
        worker_policy = {};
    }

    HRESULT start_wheel(HWND source) {
        DWORD pid = 0;
        wchar_t name[64]{};

        if (!IsWindow(source) || !GetWindowThreadProcessId(source, &pid) || pid != GetCurrentProcessId()) return E_INVALIDARG;
        if (!GetClassNameW(source, name, 64) || lstrcmpW(name, L"UnityWndClass") != 0) return E_INVALIDARG;

        if (!TryAcquireSRWLockExclusive(&wheel_lifetime)) return S_FALSE;

        HRESULT result = S_OK;
        if (wheel_thread) {
            result = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        } else {
            wheel_epoch.store(0);
            wheel_enabled.store(false);
            wheel_fault.store(false);
            wheel_error.store(0);

            AcquireSRWLockExclusive(&gate);
            wheel_start_policy_revision = policy.revision;
            ReleaseSRWLockExclusive(&gate);

            wheel_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!wheel_stop) {
                result = HRESULT_FROM_WIN32(GetLastError());
            } else {
                wheel_window = source;
                wheel_source.store(reinterpret_cast<uintptr_t>(source));
                ++wheel_generation;
                wheel_state.store(1);
                wheel_enabled.store(true);

                wheel_thread = CreateThread(nullptr, 0, wheel_pump, nullptr, 0, nullptr);
                if (!wheel_thread) {
                    result = HRESULT_FROM_WIN32(GetLastError());
                    CloseHandle(wheel_stop);
                    wheel_stop = nullptr;
                    wheel_enabled.store(false);
                    wheel_state.store(4);
                    wheel_error.store(result);
                }
            }
        }

        ReleaseSRWLockExclusive(&wheel_lifetime);
        return result;
    }

    HRESULT stop_wheel(uint32_t timeout_ms) {
        if (timeout_ms != 0) return E_INVALIDARG;
        if (!TryAcquireSRWLockExclusive(&wheel_lifetime)) return S_FALSE;

        wheel_enabled.store(false, std::memory_order_release);
        wheel_epoch.store(0, std::memory_order_release);

        HRESULT result = S_OK;
        if (wheel_thread) {
            SetEvent(wheel_stop);
            const DWORD wait = WaitForSingleObject(wheel_thread, 0);
            if (wait == WAIT_TIMEOUT) {
                result = S_FALSE;
            } else if (wait != WAIT_OBJECT_0) {
                result = HRESULT_FROM_WIN32(GetLastError());
            } else {
                CloseHandle(wheel_thread);
                CloseHandle(wheel_stop);
                wheel_thread = nullptr;
                wheel_stop = nullptr;
                result = wheel_error.load();
            }
        }

        ReleaseSRWLockExclusive(&wheel_lifetime);
        return result;
    }

    HRESULT wheel_status(smf_control_wheel_status& out) {
        out = {sizeof(out), 1, wheel_state.load(), wheel_error.load(), wheel_source.load(), wheel_generation.load(),
            wheel_observed.load(), wheel_events.count(), wheel_consumed.load(), wheel_denied.load(), wheel_stale.load(),
            wheel_overflow.load(), wheel_epoch.load(), wheel_thread_id.load(), 0};
        return S_OK;
    }

}

SMF_CONTROL_API smf_camera_control_wheel_status(smf_control_wheel_status* out, uint32_t bytes) {
    if (!out || bytes != sizeof(*out)) return E_INVALIDARG;
    return camera_control::wheel_status(*out);
}
