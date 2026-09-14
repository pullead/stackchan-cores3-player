#pragma once

#include <cstdint>

namespace media {

inline constexpr uint8_t kMutedVolumePercent = 0;

constexpr uint8_t startup_volume(int) noexcept {
    return 0;
}

constexpr uint8_t test_volume(int) noexcept {
    return 0;
}

constexpr uint8_t clamp_user_volume(int value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return static_cast<uint8_t>(value);
}

// Unlike the upstream AI codec startup policy, zero is an intentional mute
// value and must survive a mode switch or reboot.
constexpr uint8_t restore_persisted_volume(int value) noexcept {
    return clamp_user_volume(value);
}

}  // namespace media
