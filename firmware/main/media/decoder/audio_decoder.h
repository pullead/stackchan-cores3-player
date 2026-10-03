#pragma once

#include "media/media_types.h"
#include "media/decoder/audio_stream.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace media {

struct PcmBlock {
    int16_t* samples = nullptr;
    std::size_t capacity_frames = 0;
    std::size_t frames = 0;

    bool valid() const noexcept {
        return samples != nullptr && capacity_frames > 0 && frames <= capacity_frames;
    }
};

struct AudioMetadata {
    std::string title;
    std::string artist;
    std::string album;
};

enum class AudioDecodeStatus : uint8_t {
    Ok,
    Eof,
    InvalidArgument,
    NotOpen,
    Unsupported,
    Malformed,
    IoError,
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    virtual AudioDecodeStatus open(AudioStream& stream) noexcept = 0;
    virtual AudioDecodeStatus decode(PcmBlock& block) noexcept = 0;
    virtual const PcmFormat& format() const noexcept = 0;
    virtual const AudioMetadata& metadata() const noexcept = 0;
    virtual bool eof() const noexcept = 0;
    virtual AudioDecodeStatus last_error() const noexcept = 0;

    // Discards whatever is buffered so the caller can reposition the stream and
    // keep decoding from there.  A backend that cannot resync leaves this
    // returning Unsupported, and the caller must then refuse to seek rather than
    // play from a position the decoder disagrees with.
    virtual AudioDecodeStatus reset() noexcept { return AudioDecodeStatus::Unsupported; }
};

}  // namespace media
