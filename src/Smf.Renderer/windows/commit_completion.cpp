#include "session_internal.h"

namespace session {
    namespace {

        struct request {
            uint64_t token = 0;
            uint64_t serial = 0;
            uint64_t session = 0;
            IDCompositionDevice* device = nullptr;
            bool started = false;
            bool done = false;
            HRESULT result = S_FALSE;
            int64_t before = 0;
            int64_t after = 0;
        };

        struct completion_state {
            SRWLOCK gate = SRWLOCK_INIT;
            std::array<request, 4> jobs{};
            HANDLE thread = nullptr;
            HANDLE wake = nullptr;
            uint64_t next = 1;
            bool stopping = false;
        };

        completion_state& completion = *new completion_state();

        DWORD WINAPI completion_run(void*) {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            {
                lock l(state().gate);
                state().status.completion_thread = GetCurrentThreadId();
            }

            for (;;) {
                request* job = nullptr;
                {
                    lock l(completion.gate);
                    for (auto& q : completion.jobs) {
                        if (q.token && !q.started && !q.done) {
                            q.started = true;
                            job = &q;
                            break;
                        }
                    }
                    if (!job && completion.stopping) break;
                }

                if (!job) {
                    WaitForSingleObject(completion.wake, INFINITE);
                    continue;
                }

                // WaitForCommitCompletion is the only device method this thread ever calls.
                const int64_t before = now();
                const HRESULT result = FAILED(initialized) ? initialized : job->device->WaitForCommitCompletion();
                const int64_t after = now();
                job->device->Release();

                {
                    lock l(completion.gate);
                    job->device = nullptr;
                    job->before = before;
                    job->after = after;
                    job->result = result;
                    job->done = true;
                }
                wake();
            }

            if (SUCCEEDED(initialized)) CoUninitialize();
            return 0;
        }

    }

    HRESULT completion_start() {
        lock l(completion.gate);
        if (completion.thread) return HRESULT_FROM_WIN32(ERROR_BUSY);

        completion.stopping = false;
        completion.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completion.wake) return HRESULT_FROM_WIN32(GetLastError());

        completion.thread = CreateThread(nullptr, 0, completion_run, nullptr, 0, nullptr);
        if (!completion.thread) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(completion.wake);
            completion.wake = nullptr;
            return hr;
        }

        return S_OK;
    }

    uint64_t completion_request(IDCompositionDevice* device, uint64_t serial, uint64_t session) {
        if (!device || !serial || !session) return 0;
        lock l(completion.gate);
        if (!completion.thread || completion.stopping) return 0;

        for (auto& q : completion.jobs) {
            if (q.token) continue;
            device->AddRef();
            q = {};
            q.token = completion.next++;
            q.serial = serial;
            q.session = session;
            q.device = device;
            SetEvent(completion.wake);
            return q.token;
        }

        return 0;
    }

    HRESULT completion_poll(uint64_t token, uint64_t* completed) {
        if (!token) return E_INVALIDARG;
        lock l(completion.gate);

        for (auto& q : completion.jobs) {
            if (q.token != token) continue;
            if (!q.done) return S_FALSE;
            const HRESULT hr = q.result;
            if (completed) *completed = hr == S_OK ? q.serial : 0;
            q = {};
            return hr;
        }

        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    bool completion_idle() {
        lock l(completion.gate);
        for (const auto& q : completion.jobs) {
            if (q.token) return false;
        }
        return true;
    }

    void completion_request_stop() {
        lock l(completion.gate);
        completion.stopping = true;
        if (completion.wake) SetEvent(completion.wake);
    }

    HRESULT completion_poll_joined() {
        lock l(completion.gate);
        if (!completion.thread) return S_OK;

        const DWORD result = WaitForSingleObject(completion.thread, 0);
        if (result == WAIT_TIMEOUT) return S_FALSE;
        if (result != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());

        CloseHandle(completion.thread);
        CloseHandle(completion.wake);
        completion.thread = nullptr;
        completion.wake = nullptr;
        return S_OK;
    }

}
