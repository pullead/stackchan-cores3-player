#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

class AudioCodecPort {
public:
    virtual ~AudioCodecPort() = default;

    virtual void set_volume(uint8_t volume) = 0;
    virtual bool enable_output(bool enabled) = 0;
    // Takes interleaved samples, not frames: the sink knows the channel count
    // and converts, so the codec layer stays format-agnostic.
    virtual size_t write_samples(const int16_t* pcm, size_t samples) = 0;
};

}  // namespace media
