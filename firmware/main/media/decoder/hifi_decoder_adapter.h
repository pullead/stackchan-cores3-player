#pragma once

#include "media/decoder/audio_decoder.h"

#include <cstdint>
#include <string>

namespace media {

// Narrow seam for ESP32-audioI2S (or another decoder) integration.  The
// adapter deliberately owns no I2S, task, or volume state.  A production
// backend must consume the supplied read-only AudioStream and report decoded
// PCM through decode().
class HifiDecoderBackend {
public:
    virtual ~HifiDecoderBackend() = default;
    virtual AudioDecodeStatus open(AudioStream& stream) noexcept = 0;
    virtual AudioDecodeStatus decode(PcmBlock& block) noexcept = 0;
    virtual const PcmFormat& format() const noexcept = 0;
    virtual const AudioMetadata& metadata() const noexcept = 0;
    virtual bool eof() const noexcept = 0;
    virtual AudioDecodeStatus last_error() const noexcept = 0;
};

// StackChan-facing adapter for the latest HiFi decoder flow.  The default
// constructor is intentionally unavailable until the decoder dependency is
// linked; use the injected backend in firmware/tests.
class HifiDecoderAdapter final : public AudioDecoder {
public:
    explicit HifiDecoderAdapter(HifiDecoderBackend& backend) noexcept
        : backend_(backend) {}

    AudioDecodeStatus open(AudioStream& stream) noexcept override;
    AudioDecodeStatus decode(PcmBlock& block) noexcept override;
    const PcmFormat& format() const noexcept override;
    const AudioMetadata& metadata() const noexcept override;
    bool eof() const noexcept override;
    AudioDecodeStatus last_error() const noexcept override;

private:
    HifiDecoderBackend& backend_;
    AudioDecodeStatus error_ = AudioDecodeStatus::NotOpen;
    bool opened_ = false;
};

}  // namespace media
