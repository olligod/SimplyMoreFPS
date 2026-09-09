#pragma once
#include "../common/camera_packets.h"
#include "../../Smf.Camera/camera_kernel.h"
#include <stdint.h>
#include <stddef.h>

#define SMF_BRIDGE_API extern "C" __declspec(dllexport)

// Every export except status runs on the Unity main thread. The compositor worker
// loads the kernel module and is the only thread that calls into it. Both modules
// stay loaded until the process exits.
SMF_BRIDGE_API double __cdecl smf_camera_bridge_now();
SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_init(const wchar_t* kernel_path, uint32_t characters);
// 0 copied, 1 busy (publish the latest bundle again next frame), <0 rejected.
SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_publish(const smf_bridge_main*, uint32_t bytes);
// Moves the fence to a new, higher epoch without waiting for the worker.
SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_revoke(uint64_t epoch, uint32_t reason);
SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_desired(smf_bridge_desired*, uint32_t bytes);
// Any thread. S_FALSE means the mailbox was busy.
SMF_BRIDGE_API int32_t __cdecl smf_camera_bridge_status(smf_bridge_status*, uint32_t bytes);

#ifdef SMF_BRIDGE_INTERNAL
#include <windows.h>

namespace camera_bridge {

    void worker_ready(uint32_t thread, int64_t frequency);
    // S_OK fills target with the pose to display; S_FALSE means keep the previous one.
    HRESULT worker_prepare(bool focused, bool pointer_valid, double x, double y, bool middle, smf_bridge_desired& target);
    // Called with the result of the worker's Commit for the prepared pose.
    void worker_committed(HRESULT result);
    // The prepared pose equals the last committed one, so no new Commit was made.
    void worker_reused_pose();
    // Releases the kernel session; must run on the worker thread before it exits.
    void worker_removed();

}
#endif
