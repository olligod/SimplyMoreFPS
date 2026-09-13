#include "session_internal.h"
#include "present_observer.h"
#include "../common/selection_overlay.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace session {

    shared& state() {
        static shared* value = new shared();
        return *value;
    }

    int64_t now() {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        return q.QuadPart;
    }

    void wake() {
        if (state().wake) SetEvent(state().wake);
    }

    bool owned_window() {
        auto& s = state();
        DWORD pid = 0;
        wchar_t name[64]{};

        return GetWindowThreadProcessId(s.window, &pid) == s.window_thread && pid == GetCurrentProcessId() &&
            GetClassNameW(s.window, name, 64) && lstrcmpW(name, L"UnityWndClass") == 0;
    }

    // Caller holds the gate.
    bool is_current(uint64_t session, uint64_t content, uint64_t generation) {
        auto& s = state();

        for (const auto& g : s.status.generations) {
            if (accept_content(s.session.load(), s.content.load(), s.captures_sealed.load(), g, session, content, generation)) {
                return true;
            }
        }
        return false;
    }

    void acknowledge(const session_command& c, HRESULT hr, uint32_t evidence, uint64_t frame, uint64_t commit, bool superseded) {
        auto& s = state();
        lock l(s.gate);

        for (const auto& old : s.acks) {
            if (old.session == c.session && old.serial == c.serial) return;
        }

        session_ack a{};
        a.size = sizeof(a);
        a.version = 1;
        a.session = c.session;
        a.serial = c.serial;
        a.generation = c.generation;
        a.content_revision = c.content_revision;
        a.source_frame = frame;
        a.operation = c.operation;
        a.evidence = evidence;
        a.result = hr;
        a.disposition = superseded ? 2u : FAILED(hr) ? 3u : 1u;
        a.commit_serial = commit;
        a.completed_qpc = now();

        s.acks[s.ack_index++ % ack_count] = a;
        s.status.last_ack_serial = c.serial;

        for (auto& op : s.operations) {
            if (op.used && op.command.serial == c.serial) op = {};
        }
    }

    namespace {

        bool on_main_thread() {
            return state().main_thread.load() == GetCurrentThreadId();
        }

        bool valid_header(const void* p, uint32_t bytes, uint32_t expected) {
            if (!p || bytes != expected) return false;
            const auto* h = static_cast<const uint32_t*>(p);
            return h[0] == expected && h[1] == 1;
        }

        ticket* ticket_at(void* address) {
            auto& tickets = state().tickets;
            const auto p = reinterpret_cast<uintptr_t>(address);
            const auto base = reinterpret_cast<uintptr_t>(tickets.data());
            const bool inside = p >= base && p < base + sizeof(tickets) && (p - base) % sizeof(ticket) == 0;

            return inside ? static_cast<ticket*>(address) : nullptr;
        }

        HRESULT allocate_ticket(ticket_kind kind, void** handle, int32_t* token, ticket*& result) {
            if (!handle || !token) return E_POINTER;
            auto& s = state();
            if (s.next_ticket > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) return E_BOUNDS;

            for (auto& t : s.tickets) {
                if (t.kind != ticket_kind::empty) continue;
                t = {};
                t.kind = kind;
                t.token = static_cast<int32_t>(s.next_ticket++);
                *handle = &t;
                *token = t.token;
                result = &t;
                ++s.status.queued_callbacks;
                return S_OK;
            }

            return S_FALSE;
        }

        void release_textures(ticket& t) {
            if (t.first) t.first->Release();
            if (t.second) t.second->Release();
            if (t.cache) t.cache->Release();
            if (t.has_scene) {
                for (uint32_t i = 0; i < t.scene.frame.image_count; ++i)
                    reinterpret_cast<ID3D11Texture2D*>(t.scene.images[i].texture)->Release();
            }
            t.first = t.second = t.cache = nullptr;
            t.has_scene = false;
        }

        struct ticket_release {
            ticket& value;
            ~ticket_release() { release_textures(value); }
        };

        // Unity render thread entry (GL.IssuePluginEventAndData).
        void __stdcall render_callback(int32_t event, void* data) noexcept {
            auto& s = state();

            try {
                {
                    lock l(s.gate);
                    if (!s.status.render_thread) s.status.render_thread = GetCurrentThreadId();
                    if (s.status.render_thread != GetCurrentThreadId()) {
                        s.status.result = HRESULT_FROM_WIN32(ERROR_INVALID_THREAD_ID);
                        return;
                    }
                }

                source_pump();

                ticket local{};
                ticket_release release{local};
                {
                    lock l(s.gate);
                    if (event) {
                        auto* t = ticket_at(data);
                        if (!t || t->token != event || t->kind == ticket_kind::empty || t->consumed) return;
                        t->consumed = true;
                        local = *t;
                        *t = {};
                        --s.status.queued_callbacks;
                    }
                }

                if (local.kind != ticket_kind::empty && !local.canceled) source_callback(local);
                release_textures(local);

                // Release one cancelled ticket per callback, outside the gate.
                {
                    lock l(s.gate);
                    local = {};
                    for (auto& t : s.tickets) {
                        if (t.kind != ticket_kind::empty && t.canceled) {
                            local = t;
                            t = {};
                            --s.status.queued_callbacks;
                            break;
                        }
                    }
                }

                if (local.canceled) release_textures(local);
                source_pump();
            } catch (...) {
                lock l(s.gate);
                s.status.result = E_FAIL;
            }
        }

    }
}

using namespace session;

SMF_SESSION_API int32_t __cdecl smf_selection_publish(const smf_selection_state* value, uint32_t bytes) {
    if (!on_main_thread()) return E_UNEXPECTED;
    return selection::state().publish(value, bytes);
}

SMF_SESSION_API int32_t __cdecl smf_session_start(uint64_t hwnd, uint64_t session) {
    auto& s = state();
    if (!hwnd || !session) return E_INVALIDARG;

    smf_po_start(hwnd); // asynchronous, process lifetime, outside the gate

    try_lock l(s.gate);
    if (!l) return S_FALSE;
    if (s.main_thread.load() && s.main_thread.load() != GetCurrentThreadId()) return HRESULT_FROM_WIN32(ERROR_INVALID_THREAD_ID);

    const bool same_window = s.window == reinterpret_cast<HWND>(hwnd);
    switch (decide_start(s.session.load(), session, s.worker != nullptr, s.captures_sealed.load(), same_window)) {
    case start_decision::already_accepted: return S_OK;
    case start_decision::busy: return S_FALSE;
    case start_decision::stale: return E_INVALIDARG;
    case start_decision::create: break;
    }

    s.window = reinterpret_cast<HWND>(hwnd);
    DWORD pid = 0;
    s.window_thread = GetWindowThreadProcessId(s.window, &pid);
    if (!owned_window() || !IsWindowVisible(s.window)) return E_INVALIDARG;

    for (const auto& t : s.tickets) {
        if (t.kind != ticket_kind::empty) return S_FALSE;
    }
    for (const auto& g : s.status.generations) {
        if (g.state && g.state != 5) return S_FALSE;
    }

    s.main_thread = GetCurrentThreadId();
    s.session = session;
    s.content = 0;
    s.operation_fence = 0;
    s.restore_after_frame = 0;
    s.native_reject_through_frame = 0;
    s.captures_sealed = false;
    s.stop_requested = false;
    s.ever_active = false;
    s.stop_result = S_OK;
    s.detach_requested = 0;
    s.detach_completed = 0;
    s.status = {};
    s.status.size = sizeof(s.status);
    s.status.version = 1;
    s.status.session = session;
    s.status.main_thread = GetCurrentThreadId();
    s.status.worker_state = 1;
    s.links = {};
    s.models = {};
    s.operations = {};
    s.last_command = 0;

    s.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.wake) return HRESULT_FROM_WIN32(GetLastError());

    const HRESULT complete = completion_start();
    if (FAILED(complete)) {
        CloseHandle(s.wake);
        s.wake = nullptr;
        return complete;
    }

    s.worker = CreateThread(nullptr, 0, worker_run, nullptr, 0, nullptr);
    if (!s.worker) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        completion_request_stop();
        CloseHandle(s.wake);
        s.wake = nullptr;
        return hr;
    }

    return S_OK;
}

SMF_SESSION_API int32_t __cdecl smf_session_content_fence(uint64_t session, uint64_t revision) {
    auto& s = state();
    if (!on_main_thread() || session != s.session.load() || !revision || s.captures_sealed.load()) return E_INVALIDARG;

    try_lock l(s.gate);
    if (!l) return S_FALSE;
    const uint64_t old = s.content.load();
    if (revision < old) return E_INVALIDARG;
    if (revision == old) return S_OK;

    // Taking the gate orders the fence after the last old-content commit.
    s.content = revision;
    wake();
    return S_OK;
}

SMF_SESSION_API int32_t __cdecl smf_session_command(const session_command* c, uint32_t bytes) {
    auto& s = state();
    if (!on_main_thread() || !valid_header(c, bytes, sizeof(*c)) || c->session != s.session.load() || !c->serial) {
        return E_INVALIDARG;
    }
    if (c->operation > op_stop_worker || c->operation == op_release_generation_main || c->operation == op_release_session_main) {
        return E_INVALIDARG;
    }

    try_lock l(s.gate);
    if (!l) return S_FALSE;

    if (c->operation == op_restore_native) {
        uint64_t old = s.operation_fence.load();
        while (old < c->serial && !s.operation_fence.compare_exchange_weak(old, c->serial)) {
        }
        if (old > c->serial) return E_INVALIDARG;
        // Captures are sealed before the command is even queued.
        s.captures_sealed = true;
        s.restore_after_frame = c->after_frame;
    }

    for (const auto& a : s.acks) {
        if (a.session == c->session && a.serial == c->serial) {
            return a.operation == c->operation && a.generation == c->generation ? S_OK : E_INVALIDARG;
        }
    }

    for (const auto& o : s.operations) {
        if (o.used && o.command.serial == c->serial) return std::memcmp(&o.command, c, sizeof(*c)) == 0 ? S_OK : E_INVALIDARG;
    }

    if (c->serial <= s.last_command) return E_INVALIDARG;
    if (c->operation < op_restore_native && s.captures_sealed.load()) return E_ABORT;

    const bool prepare = c->operation == op_prepare_hidden || c->operation == op_prepare_replacement;
    if (prepare) {
        if (!c->generation || c->content_revision != s.content.load()) return E_INVALIDARG;
        if (c->width < 1 || c->height < 1 || c->width > 16384 || c->height > 16384) return E_INVALIDARG;
        if (uint64_t(c->width) * c->height * 4 > 64ull * 1024 * 1024 || (c->flags & ~session_has_map)) return E_INVALIDARG;

        for (float v : c->empty_background) {
            if (!std::isfinite(v) || v < 0 || v > 1) return E_INVALIDARG;
        }

        if (c->empty_background[3] != 1) return E_INVALIDARG;
        for (const auto& g : s.status.generations) {
            if (g.generation == c->generation && g.state) return E_INVALIDARG;
        }
    }

    for (auto& op : s.operations) {
        if (op.used) continue;

        if (prepare) {
            session_generation_status* slot = nullptr;

            for (auto& g : s.status.generations) {
                if (!g.state || g.state == 5) {
                    slot = &g;
                    break;
                }
            }

            if (!slot) return S_FALSE;
            *slot = {};
            slot->generation = c->generation;
            slot->content_revision = c->content_revision;
            slot->width = c->width;
            slot->height = c->height;
            slot->flags = c->flags;
            slot->state = 1;
        }

        op = {};
        op.command = *c;
        op.used = true;
        if (prepare) op.phase = 1;
        s.last_command = c->serial;
        wake();
        return S_OK;
    }
    return S_FALSE;
}

SMF_SESSION_API int32_t __cdecl smf_session_ack(uint64_t session, uint64_t serial, session_ack* out, uint32_t bytes) {
    if (!out || bytes != sizeof(*out)) return E_INVALIDARG;
    auto& s = state();
    try_lock l(s.gate);
    if (!l) return S_FALSE;

    for (const auto& a : s.acks) {
        if (a.session == session && a.serial == serial) {
            *out = a;
            return S_OK;
        }
    }

    return S_FALSE;
}

SMF_SESSION_API int32_t __cdecl smf_session_status(session_status* out, uint32_t bytes) {
    if (!out || bytes != sizeof(*out)) return E_INVALIDARG;
    auto& s = state();
    try_lock l(s.gate);
    if (!l) return S_FALSE;

    *out = s.status;
    out->content_fence = s.content.load();
    out->operation_fence = s.operation_fence.load();
    if (out->native_submitted_frame <= s.native_reject_through_frame.load()) out->flags &= ~2u;
    return S_OK;
}

SMF_SESSION_API int32_t __cdecl smf_session_pre_gui(const session_pre_gui* p, uint32_t bytes, void** ticket, int32_t* token) {
    if (!on_main_thread() || !valid_header(p, bytes, sizeof(*p)) || !p->bootstrap_texture || p->reserved || (p->flags & ~session_has_map)) {
        return E_INVALIDARG;
    }

    auto& s = state();
    try_lock l(s.gate);
    if (!l) return S_FALSE;
    if (!is_current(p->session, p->content_revision, p->generation)) return E_ABORT;

    session::ticket* t = nullptr;
    const HRESULT hr = allocate_ticket(ticket_kind::pre_gui, ticket, token, t);
    if (hr != S_OK) return hr;

    t->pre = *p;
    t->first = reinterpret_cast<ID3D11Texture2D*>(p->bootstrap_texture);
    t->first->AddRef();
    return S_OK;
}

SMF_SESSION_API int32_t __cdecl smf_session_frame(const session_frame* input, uint32_t bytes, void** ticket, int32_t* token) {
    if (!on_main_thread() || !input || bytes < 8 || !frame_layout_matches(bytes, input->size, input->version)) return E_INVALIDARG;

    session_frame normalized{};
    std::memcpy(&normalized, input, bytes);
    const auto* p = &normalized;

    if (!p->world_texture || !p->hud_texture || (p->flags & ~31u)) return E_INVALIDARG;
    const bool complete = (p->flags & session_world_dispatch_complete) != 0;
    const bool absent = (p->flags & session_world_dispatch_absent) != 0;
    if (complete == absent || (absent && p->world_dispatches) || (complete && !p->world_dispatches)) return E_INVALIDARG;

    if (p->flags & session_has_map) {
        if (p->pose.size != sizeof(p->pose) || p->pose.version != 2 || p->pose.unity_frame != p->source_frame) return E_INVALIDARG;
    } else {
        const session_pose zero{};
        if (std::memcmp(&p->pose, &zero, sizeof(zero)) || !absent) return E_INVALIDARG;
    }

    auto& s = state();
    try_lock l(s.gate);
    if (!l) return S_FALSE;
    if (!is_current(p->session, p->content_revision, p->generation)) return E_ABORT;
    smf_scene::snapshot scene{};
    if (p->scene_description) {
        if (!(p->flags & session_has_map) || !smf_scene::read_snapshot(
            reinterpret_cast<const smf_scene::description*>(p->scene_description), p->source_frame,
            static_cast<uint32_t>(p->pose.pixel_width), static_cast<uint32_t>(p->pose.pixel_height), scene)) return E_INVALIDARG;
    } else if (!cache_valid(*p)) return E_INVALIDARG;

    session::ticket* t = nullptr;
    const HRESULT hr = allocate_ticket(ticket_kind::frame, ticket, token, t);
    if (hr != S_OK) return hr;

    t->frame = *p;
    t->first = reinterpret_cast<ID3D11Texture2D*>(p->world_texture);
    t->second = reinterpret_cast<ID3D11Texture2D*>(p->hud_texture);
    t->cache = p->scene_description ? nullptr : reinterpret_cast<ID3D11Texture2D*>(p->cache.texture);
    t->has_scene = p->scene_description != 0;
    if (t->has_scene) {
        t->scene = scene;
        t->frame.scene_description = 0;
        for (uint32_t i = 0; i < scene.frame.image_count; ++i)
            reinterpret_cast<ID3D11Texture2D*>(scene.images[i].texture)->AddRef();
    }
    t->first->AddRef();
    t->second->AddRef();
    if (t->cache) t->cache->AddRef();
    return S_OK;
}

SMF_SESSION_API int32_t __cdecl smf_session_native_frame(const session_native_frame* p, uint32_t bytes, void** ticket, int32_t* token) {
    auto& s = state();
    if (!on_main_thread() || !valid_header(p, bytes, sizeof(*p)) || p->session != s.session.load()) return E_INVALIDARG;
    if ((p->flags & ~1u) || p->reserved || !p->width || !p->height) return E_INVALIDARG;

    if (p->restore_serial) {
        if (p->restore_serial != s.operation_fence.load()) return E_INVALIDARG;
        if (!(p->flags & 1u) && p->source_frame <= s.restore_after_frame.load()) return E_INVALIDARG;
    }

    // A frame we could not queue must never count as a native handoff later.
    const auto reject = [&]() {
        uint64_t old = s.native_reject_through_frame.load();
        while (old < p->source_frame && !s.native_reject_through_frame.compare_exchange_weak(old, p->source_frame)) {
        }
    };

    try_lock l(s.gate);
    if (!l) {
        reject();
        return S_FALSE;
    }

    session::ticket* t = nullptr;
    const HRESULT hr = allocate_ticket(ticket_kind::native_frame, ticket, token, t);
    if (hr == S_OK) {
        t->native = *p;
    } else {
        reject();
        s.status.flags &= ~2u;
    }

    return hr;
}

SMF_SESSION_API int32_t __cdecl smf_session_cancel(void* handle, int32_t token) {
    if (!on_main_thread()) return E_UNEXPECTED;
    auto& s = state();
    try_lock l(s.gate);
    if (!l) return S_FALSE;

    auto* t = ticket_at(handle);
    if (!t) return E_INVALIDARG;

    switch (decide_cancel(token, s.next_ticket, t->kind == ticket_kind::empty, t->token)) {
    case cancel_decision::invalid: return E_INVALIDARG;
    case cancel_decision::complete: return S_OK;
    case cancel_decision::mark:
        t->canceled = true;
        return S_OK;
    }

    return E_UNEXPECTED;
}

SMF_SESSION_API void* __cdecl smf_session_render_event() {
    return reinterpret_cast<void*>(&render_callback);
}

SMF_SESSION_API int32_t __cdecl smf_session_poll_joined(uint64_t session) {
    if (!on_main_thread() || session != state().session.load()) return E_INVALIDARG;

    const HRESULT hr = worker_poll_joined();
    if (hr != S_OK) return hr;
    return completion_poll_joined();
}
