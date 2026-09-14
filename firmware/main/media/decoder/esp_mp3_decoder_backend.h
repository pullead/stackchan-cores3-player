#pragma once

#include "media/decoder/hifi_decoder_adapter.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace media {

// ESP-IDF esp_audio_codec MP3 bridge.  It owns only decoder state and an
// encoded-byte staging buffer; I2S, volume, and storage remain elsewhere.
class EspMp3DecoderBackend final : public HifiDecoderBackend {
public:
    EspMp3DecoderBackend() noexcept = default;
    ~EspMp3DecoderBackend() override;

    AudioDecodeStatus open(AudioStream& stream) noexcept override;
    AudioDecodeStatus decode(PcmBlock& block) noexcept override;
    const PcmFormat& format() const noexcept override { return format_; }
    const AudioMetadata& metadata() const noexcept override { return metadata_; }
    bool eof() const noexcept override { return eof_; }
    AudioDecodeStatus last_error() const noexcept override { return error_; }

private:
    void reset_state() noexcept;
    AudioDecodeStatus map_error(int error) const noexcept;
    void* decoder_ = nullptr;
    AudioStream* stream_ = nullptr;
    std::array<uint8_t, 16384> input_{};
    std::size_t input_size_ = 0;
    bool source_eof_ = false;
    bool eof_ = false;
    PcmFormat format_{};
    AudioMetadata metadata_{};
    AudioDecodeStatus error_ = AudioDecodeStatus::NotOpen;
};

HifiDecoderBackend* create_esp_mp3_decoder_backend() noexcept;

}  // namespace media
