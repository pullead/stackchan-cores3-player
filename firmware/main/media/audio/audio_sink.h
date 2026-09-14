#pragma once

#include <cstddef>
#include <cstdint>

#include "media/media_types.h"

namespace media {

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool open(const PcmFormat& format) = 0;
    virtual size_t write(const int16_t* pcm, size_t frames) = 0;
    virtual void pause() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
};

}  // namespace media
