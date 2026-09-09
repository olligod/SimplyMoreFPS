#pragma once
#include "../common/camera_packets.h"
#include "../../Smf.Camera/camera_kernel.h"
#include <stdint.h>
#include <stddef.h>

#define SMF_CONTROL_API extern "C" __declspec(dllexport) int32_t __cdecl

// Main thread only, same owner as the camera bridge. 0 copied, 1 busy (a policy may
// be retried, an impulse may not), <0 rejected.
SMF_CONTROL_API smf_camera_control_policy(const smf_control_policy*, uint32_t);
SMF_CONTROL_API smf_camera_control_impulse(const smf_control_impulse*, uint32_t);
SMF_CONTROL_API smf_camera_control_status(smf_control_status*, uint32_t);
SMF_CONTROL_API smf_camera_control_wheel_status(smf_control_wheel_status*, uint32_t);

#ifdef SMF_BRIDGE_INTERNAL
#include <windows.h>

namespace camera_control {

    HRESULT publish_policy(const smf_control_policy&);
    HRESULT queue(const smf_control_impulse&);
    HRESULT read_status(smf_control_status&);
    // Worker thread, once per acknowledged kernel step.
    void prepare(uint64_t epoch, bool blocked, double x, double y, smf_camera_input&);
    void clear();
    // The wheel hook only observes; it never registers raw input or swallows events.
    HRESULT start_wheel(HWND source);
    // Only timeout 0 is accepted; poll until it returns something other than S_FALSE.
    HRESULT stop_wheel(uint32_t timeout_ms);
    HRESULT wheel_status(smf_control_wheel_status&);

}
#endif
