#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

class AudioCodecPort {
public:
    virtual ~AudioCodecPort() = default;

    virtual void set_volume(uint8_t volume) = 0;
    virtual bool enable_output(bool enabled) = 0;
    virtual size_t write_mono(const int16_t* pcm, size_t frames) = 0;
};

}  // namespace media
