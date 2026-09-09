// CPU-only reproduction of the DXGI E9 -> private FF25 -> overlay detour chain seen
// with the Steam overlay. No D3D, swapchain, window or hook installation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../vendor/minhook/include/MinHook.h"

using function = int(__cdecl*)(int, int);

static function previous = nullptr;
static volatile int overlay_calls = 0;
static volatile int observer_calls = 0;

__declspec(noinline) static int original(int a, int b) {
    volatile int value = a * 17;
    return value + b * 3;
}

__declspec(noinline) static int overlay(int a, int b) {
    ++overlay_calls;
    return a * 17 + b * 3;
}

__declspec(noinline) static int observer(int a, int b) {
    ++observer_calls;
    return previous(a, b);
}

int main() {
    const uintptr_t target = reinterpret_cast<uintptr_t>(&original);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const uintptr_t step = system.dwAllocationGranularity;

    // A relay page within rel32 range of the target, holding an FF25 jump to the overlay.
    void* relay = nullptr;
    for (uintptr_t distance = step; distance < 0x10000000 && !relay; distance += step) {
        relay = VirtualAlloc(reinterpret_cast<void*>((target & ~(step - 1)) + distance), 4096,
            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }

    if (!relay) {
        std::puts("FAIL relay allocation");
        return 1;
    }

    uint8_t jump[14]{0xFF, 0x25, 0, 0, 0, 0};
    const uintptr_t overlay_address = reinterpret_cast<uintptr_t>(&overlay);
    std::memcpy(jump + 6, &overlay_address, 8);
    std::memcpy(relay, jump, sizeof(jump));

    // Patch the target with an E9 to the relay, like an overlay would.
    uint8_t saved[5]{};
    std::memcpy(saved, reinterpret_cast<void*>(target), 5);
    uint8_t branch[5]{0xE9};
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(relay) - target - 5);
    std::memcpy(branch + 1, &displacement, 4);

    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &protect)) return 2;
    std::memcpy(reinterpret_cast<void*>(target), branch, 5);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), 5, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);

    function volatile call = &original;
    if (call(7, 11) != 152 || overlay_calls != 1) return 3;

    if (MH_Initialize() != MH_OK) return 4;
    if (MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&observer), reinterpret_cast<void**>(&previous)) != MH_OK) return 4;
    if (MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) return 4;
    if (call(13, 19) != 278 || overlay_calls != 2 || observer_calls != 1) return 5;

    // Only the test disables its hook; it must restore the overlay's E9 byte for byte.
    if (MH_DisableHook(reinterpret_cast<void*>(target)) != MH_OK) return 6;
    if (std::memcmp(reinterpret_cast<void*>(target), branch, 5) || call(-4, 9) != -41 || overlay_calls != 3) return 7;

    MH_Uninitialize();
    VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &ignored);
    std::memcpy(reinterpret_cast<void*>(target), saved, 5);
    VirtualProtect(reinterpret_cast<void*>(target), 5, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);
    VirtualFree(relay, 0, MEM_RELEASE);

    std::puts("PASS MinHook chains through an existing E9/FF25 overlay detour and restores it exactly");
    return 0;
}
