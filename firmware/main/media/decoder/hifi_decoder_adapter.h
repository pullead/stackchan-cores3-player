#pragma once

#include "media/decoder/audio_decoder.h"

#include <cstdint>
#include <memory>
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

// StackChan-facing adapter for the latest HiFi decoder flow.  The adapter must
// outlive a borrowed backend; the owning constructor exists so the backend
// factory result cannot be leaked by a track change.  Neither form touches
// I2S, tasks or volume.
class HifiDecoderAdapter final : public AudioDecoder {
public:
    // Borrowed backend: the caller owns it and must keep it alive for the
    // adapter's lifetime (host tests inject a fixture this way).
    explicit HifiDecoderAdapter(HifiDecoderBackend& backend) noexcept
        : backend_(&backend) {}
    // Owned backend: what the firmware factory hands over.  Without this the
    // backend object and its codec handle were never freed, so every track
    // change leaked them.
    explicit HifiDecoderAdapter(std::unique_ptr<HifiDecoderBackend> backend) noexcept
        : owned_(std::move(backend)), backend_(owned_.get()) {}

    AudioDecodeStatus open(AudioStream& stream) noexcept override;
    AudioDecodeStatus decode(PcmBlock& block) noexcept override;
    const PcmFormat& format() const noexcept override;
    const AudioMetadata& metadata() const noexcept override;
    bool eof() const noexcept override;
    AudioDecodeStatus last_error() const noexcept override;

private:
    std::unique_ptr<HifiDecoderBackend> owned_;
    HifiDecoderBackend* backend_ = nullptr;
    AudioDecodeStatus error_ = AudioDecodeStatus::NotOpen;
    bool opened_ = false;
};

// Returns an owned backend, or an empty pointer when the compressed-audio
// backend is not built in.  A build without it reports nothing rather than
// pretending that compressed audio was decoded.
std::unique_ptr<HifiDecoderBackend> create_hifi_decoder_backend() noexcept;

}  // namespace media
