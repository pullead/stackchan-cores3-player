#include "media/decoder/esp_mp3_decoder_backend.h"

#include "decoder/impl/esp_mp3_dec.h"
#include "esp_audio_types.h"

#include <algorithm>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include <esp_log.h>

#define TAG "EspMp3Decoder"
#endif

namespace media {

EspMp3DecoderBackend::~EspMp3DecoderBackend() { reset_state(); }

void EspMp3DecoderBackend::reset_state() noexcept {
    if (decoder_ != nullptr) {
        esp_mp3_dec_close(decoder_);
        decoder_ = nullptr;
    }
    stream_ = nullptr;
    input_size_ = 0;
    source_eof_ = false;
    eof_ = false;
    format_ = {};
    metadata_ = {};
}

AudioDecodeStatus EspMp3DecoderBackend::map_error(int error) const noexcept {
    if (error == ESP_AUDIO_ERR_INVALID_PARAMETER) return AudioDecodeStatus::InvalidArgument;
    if (error == ESP_AUDIO_ERR_NOT_SUPPORT) return AudioDecodeStatus::Unsupported;
    if (error == ESP_AUDIO_ERR_DATA_LACK || error == ESP_AUDIO_ERR_CONTINUE) return AudioDecodeStatus::Ok;
    if (error == ESP_AUDIO_ERR_MEM_LACK) return AudioDecodeStatus::IoError;
    if (error == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) return AudioDecodeStatus::InvalidArgument;
    return AudioDecodeStatus::Malformed;
}

AudioDecodeStatus EspMp3DecoderBackend::open(AudioStream& stream) noexcept {
    reset_state();
    if (!stream.is_open()) { error_ = AudioDecodeStatus::InvalidArgument; return error_; }
    void* handle = nullptr;
    const auto result = esp_mp3_dec_open(nullptr, 0, &handle);
    if (result != ESP_AUDIO_ERR_OK || handle == nullptr) {
        error_ = map_error(result);
        return error_;
    }
    decoder_ = handle;
    stream_ = &stream;
    error_ = AudioDecodeStatus::Ok;
    return error_;
}

AudioDecodeStatus EspMp3DecoderBackend::decode(PcmBlock& block) noexcept {
    if (decoder_ == nullptr || stream_ == nullptr) { error_ = AudioDecodeStatus::NotOpen; return error_; }
    if (!block.valid()) { error_ = AudioDecodeStatus::InvalidArgument; return error_; }
    block.frames = 0;
    // PcmBlock's backing storage is sized for two int16 samples per frame by
    // LocalPlaybackController, even before the first MP3 header reveals the
    // channel count.  Never hand the codec a mono-sized buffer for stereo.
    const std::size_t output_bytes = block.capacity_frames * 2 * sizeof(int16_t);

    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (input_size_ == 0 && !source_eof_) {
            std::size_t count = 0;
            const auto status = stream_->read(input_.data(), input_.size(), count);
            input_size_ = count;
            if (status == AudioStreamStatus::IoError || status == AudioStreamStatus::Closed) {
                error_ = AudioDecodeStatus::IoError; return error_;
            }
            if (status == AudioStreamStatus::Eof || count == 0) source_eof_ = true;
        }
        if (input_size_ == 0 && source_eof_) { eof_ = true; error_ = AudioDecodeStatus::Eof; return error_; }

        esp_audio_dec_in_raw_t raw{};
        raw.buffer = input_.data(); raw.len = static_cast<uint32_t>(input_size_);
        esp_audio_dec_out_frame_t out{};
        out.buffer = reinterpret_cast<uint8_t*>(block.samples);
        out.len = static_cast<uint32_t>(output_bytes);
        esp_audio_dec_info_t info{};
        const auto result = esp_mp3_dec_decode(decoder_, &raw, &out, &info);
        const std::size_t consumed = std::min<std::size_t>(raw.consumed, input_size_);
        if (consumed != 0) {
            input_size_ -= consumed;
            std::memmove(input_.data(), input_.data() + consumed, input_size_);
        }
        if (result == ESP_AUDIO_ERR_OK && out.decoded_size != 0) {
            if (out.decoded_size > out.len || info.channel < 1 || info.channel > 2 ||
                info.sample_rate == 0 || info.bits_per_sample != 16) {
                error_ = AudioDecodeStatus::Malformed;
                return error_;
            }
            if ((out.decoded_size % (sizeof(int16_t) * info.channel)) != 0) {
                error_ = AudioDecodeStatus::Malformed;
                return error_;
            }
            if (info.sample_rate != 0) format_.sample_rate = info.sample_rate;
            format_.channels = info.channel;
            format_.bits_per_sample = info.bits_per_sample;
            block.frames = out.decoded_size / (sizeof(int16_t) * format_.channels);
            if (block.frames > block.capacity_frames) {
                error_ = AudioDecodeStatus::InvalidArgument;
                return error_;
            }
            error_ = AudioDecodeStatus::Ok;
            return error_;
        }
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // The caller owns the fixed PcmBlock storage.  Treat a codec
            // request for a larger frame as an explicit contract failure,
            // rather than silently labelling it a malformed MP3.
#ifdef ESP_PLATFORM
            ESP_LOGE(TAG, "Output buffer too small: %u bytes offered for one frame",
                     static_cast<unsigned>(output_bytes));
#endif
            error_ = AudioDecodeStatus::InvalidArgument;
            return error_;
        }
        if (result == ESP_AUDIO_ERR_DATA_LACK || result == ESP_AUDIO_ERR_CONTINUE ||
            (result == ESP_AUDIO_ERR_OK && out.decoded_size == 0)) {
            if (source_eof_) {
                // Data remaining at EOF that cannot form a frame is a
                // truncated/corrupt MP3, not a clean end-of-file.
                if (input_size_ != 0) { error_ = AudioDecodeStatus::Malformed; return error_; }
                eof_ = true; error_ = AudioDecodeStatus::Eof; return error_;
            }
            if (input_size_ == input_.size()) { error_ = AudioDecodeStatus::Malformed; return error_; }
            std::size_t count = 0;
            const auto status = stream_->read(input_.data() + input_size_, input_.size() - input_size_, count);
            input_size_ += count;
            if (status == AudioStreamStatus::Eof || count == 0) source_eof_ = true;
            if (status == AudioStreamStatus::IoError || status == AudioStreamStatus::Closed) {
                error_ = AudioDecodeStatus::IoError; return error_;
            }
            continue;
        }
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Decode failed: codec status %d, consumed %u, buffered %u",
                 static_cast<int>(result), static_cast<unsigned>(consumed),
                 static_cast<unsigned>(input_size_));
#endif
        error_ = map_error(result);
        return error_;
    }
#ifdef ESP_PLATFORM
    ESP_LOGE(TAG, "Gave up after 4 attempts with %u bytes buffered",
             static_cast<unsigned>(input_size_));
#endif
    error_ = AudioDecodeStatus::Malformed;
    return error_;
}

HifiDecoderBackend* create_esp_mp3_decoder_backend() noexcept {
    return new (std::nothrow) EspMp3DecoderBackend();
}

}  // namespace media
