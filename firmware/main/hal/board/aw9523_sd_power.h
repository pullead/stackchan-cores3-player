#pragma once

#include <cstdint>

namespace board::aw9523 {

constexpr uint8_t kSdEnableMask = static_cast<uint8_t>(1u << 4);

constexpr uint8_t sd_enable_config(uint8_t config) {
    return static_cast<uint8_t>(config & static_cast<uint8_t>(~kSdEnableMask));
}

constexpr uint8_t sd_enable_output(uint8_t output) {
    return static_cast<uint8_t>(output | kSdEnableMask);
}

}  // namespace board::aw9523
