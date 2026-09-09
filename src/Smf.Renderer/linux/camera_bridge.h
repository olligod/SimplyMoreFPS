#pragma once
#include "clock.h"
#include "../common/camera_packets.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cstdint>
#include <mutex>

// The bridge mirrors the Windows result codes so the shared bridge test reads the same.
using HRESULT = int32_t;
constexpr int S_OK = 0;
constexpr int S_FALSE = 1;
constexpr int E_UNEXPECTED = -10;
constexpr int E_INVALIDARG = -11;
constexpr int E_NOTIMPL = -12;
constexpr int E_FAIL = -13;
constexpr int E_NOINTERFACE = -14;

inline bool FAILED(int r) {
    return r < 0;
}

#define SMF_BRIDGE_API extern "C" __attribute__((visibility("default")))
#define SMF_CONTROL_API SMF_BRIDGE_API int32_t

// Main-thread entry points P/Invoked by the mod.
SMF_BRIDGE_API double smf_camera_bridge_now();
SMF_BRIDGE_API int32_t smf_camera_bridge_init(const char* path, uint32_t characters);
SMF_BRIDGE_API int32_t smf_camera_bridge_publish(const smf_bridge_main* value, uint32_t bytes);
SMF_BRIDGE_API int32_t smf_camera_bridge_revoke(uint64_t epoch, uint32_t reason);
SMF_BRIDGE_API int32_t smf_camera_bridge_desired(smf_bridge_desired* value, uint32_t bytes);
SMF_BRIDGE_API int32_t smf_camera_bridge_status(smf_bridge_status* value, uint32_t bytes);

// Worker-thread side of the camera bridge: steps the NativeAOT kernel and
// reports the desired pose back to main.
namespace camera_bridge {
    void worker_ready(uint32_t thread, int64_t frequency);
    HRESULT worker_prepare(bool focused, bool pointer_valid, double x, double y, bool middle, smf_bridge_desired& target);
    void worker_committed(HRESULT result);
    void worker_reused_pose();
    void worker_removed();
}

// Keyboard, pointer and wheel input observed on the worker's private X connection.
namespace camera_control {
    HRESULT publish_policy(const smf_control_policy&);
    HRESULT queue(const smf_control_impulse&);
    HRESULT read_status(smf_control_status&);
    HRESULT wheel_status(smf_control_wheel_status&);
    void prepare(uint64_t epoch, bool blocked, double x, double y, smf_camera_input&);
    void clear();
    bool held(int keysym);
    // Uses the worker's own connection; nothing is removed from Unity's event queue.
    bool observe(Display*, Window, double& x, double& y, bool& middle, bool& focused, bool& left);
}
