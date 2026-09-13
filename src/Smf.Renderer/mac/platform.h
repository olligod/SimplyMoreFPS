#pragma once
#include "mac_backend.h"
#include "core.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <mach/mach_time.h>
#include <pthread.h>
#include <dlfcn.h>

inline uint32_t native_thread() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return uint32_t(id);
}

// mach_absolute_time converted to nanoseconds.
inline int64_t native_now() {
    static const auto timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return int64_t((long double)mach_absolute_time() * timebase.numer / timebase.denom);
}

// The bridge and control results use the same codes on every platform.
using HRESULT = int32_t;
constexpr int S_OK = 0;
constexpr int S_FALSE = 1;
constexpr int E_UNEXPECTED = -10;
constexpr int E_INVALIDARG = -11;
constexpr int E_NOTIMPL = -12;
constexpr int E_FAIL = -13;
constexpr int E_NOINTERFACE = -14;
constexpr int E_OUTOFMEMORY = static_cast<int32_t>(0x8007000eu);

inline bool FAILED(int n) { return n < 0; }

#define SMF_BRIDGE_API SMF_MAC_API
#define SMF_CONTROL_API SMF_MAC_API int32_t

// Compositor worker side of the camera bridge (mac_camera.cpp).
namespace camera_bridge {
    void worker_ready(uint32_t thread, int64_t frequency);
    HRESULT worker_prepare(bool focused, bool pointer_valid, double x, double y, bool middle, smf_bridge_desired& desired);
    void worker_committed(HRESULT result);
    void worker_removed();
}

// Input observation: wheel tap, key state and pointer geometry (mac_input.mm).
namespace camera_control {
    HRESULT publish_policy(const smf_control_policy&);
    HRESULT queue(const smf_control_impulse&);
    HRESULT read_status(smf_control_status&);
    HRESULT wheel_status(smf_control_wheel_status&);
    void prepare(uint64_t epoch, bool motion_blocked, double x, double y, smf_camera_input& input);
    void clear();
    bool held(int quartz_key);
    // Main creates and removes the geometry state; the tap and compositor read copies.
    int start(uint64_t window_number);
    void publish_geometry(double x, double y, double width, double height, double scale_x, double scale_y);
    bool observe(double& x, double& y, bool& middle, bool& focus, bool& left);
    void keyboard_focus(bool camera_keys_allowed);
    void stop();
    bool join();
}
