#pragma once

#include <cstdint>

namespace media {

enum class MediaSource : uint8_t {
    None,
    LocalFile,
    Radio,
};

enum class PlaybackState : uint8_t {
    Idle,
    Preparing,
    Buffering,
    Playing,
    Paused,
    Error,
    Stopping,
    PreparingForAi,
};

struct PcmFormat {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;

    constexpr bool valid() const noexcept {
        return sample_rate > 0 && (channels == 1 || channels == 2) && bits_per_sample == 16;
    }
};

}  // namespace media
