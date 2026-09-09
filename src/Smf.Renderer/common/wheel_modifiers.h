#pragma once
#include <cstdint>

namespace camera_control {

    // Same bits as the wheel modifier flags in smf_control_policy::flags.
    constexpr uint32_t wheel_control = 16;
    constexpr uint32_t wheel_alt = 32;
    constexpr uint32_t wheel_shift = 64;

    inline uint32_t wheel_modifiers(bool control, bool alt, bool shift) {
        return (control ? wheel_control : 0u) | (alt ? wheel_alt : 0u) | (shift ? wheel_shift : 0u);
    }

    inline bool wheel_modifiers_allowed(uint32_t policy_flags, uint32_t observed_modifiers) {
        return (policy_flags & observed_modifiers & (wheel_control | wheel_alt | wheel_shift)) == 0;
    }

}
