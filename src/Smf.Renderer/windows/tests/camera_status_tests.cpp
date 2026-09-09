#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../camera_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main() {
    // A status read from another thread must not claim main-thread ownership.
    HRESULT observed = E_FAIL;
    smf_bridge_status status{};
    std::thread first([&] { observed = smf_camera_bridge_status(&status, sizeof(status)); });
    first.join();

    check(observed == S_OK, "initial background status observation");
    check(smf_camera_bridge_revoke(1, 0) == S_OK, "real main still acquires ownership after observer");

    HRESULT desired_result = S_OK;
    std::thread second([&] {
        observed = smf_camera_bridge_status(&status, sizeof(status));
        smf_bridge_desired desired{};
        desired_result = smf_camera_bridge_desired(&desired, sizeof(desired));
    });
    second.join();
    check(observed == S_OK && status.fence_epoch == 1, "background observes locked status and atomic fence");
    check(desired_result == E_UNEXPECTED, "desired read keeps main owner guard");
    check(status.version == 2 && status.size == 232, "status ABI header");

    const double t = smf_camera_bridge_now();
    check(t > 0 && smf_camera_bridge_now() >= t, "native trajectory clock monotonic");

    smf_bridge_main main{};
    main.size = sizeof(main);
    main.version = 2;
    main.publication = 1;
    main.state.version = 2;
    main.state.size = sizeof(main.state);
    main.state.epoch = 1;
    main.state.map_id = 0;
    main.state.projection_half_height = 35.9591827;
    main.settings.version = 2;
    main.settings.size = sizeof(main.settings);
    main.settings.revision = 1;
    main.bindings.version = 1;
    main.bindings.size = sizeof(main.bindings);
    main.bindings.revision = 1;

    main.version = 1;
    check(smf_camera_bridge_publish(&main, sizeof(main)) == E_INVALIDARG, "old main ABI rejected");
    main.version = 2;
    main.state.projection_half_height = 0;
    check(smf_camera_bridge_publish(&main, sizeof(main)) == E_INVALIDARG, "invalid actual projection rejected");
    main.state.projection_half_height = 35.9591827;
    main.trajectory.kind = 1;
    check(smf_camera_bridge_publish(&main, sizeof(main)) == E_INVALIDARG, "partial zero-id trajectory rejected");
    main.trajectory = {};
    check(smf_camera_bridge_publish(&main, sizeof(main)) == S_OK, "atomic bundle accepted");

    check(smf_camera_bridge_status(nullptr, sizeof(status)) == E_INVALIDARG, "invalid status pointer rejected");
    check(smf_camera_bridge_status(&status, sizeof(status) - 1) == E_INVALIDARG, "invalid status size rejected");

    std::puts("PASS camera status observation before and after main ownership; desired guard kept");
    return 0;
}
