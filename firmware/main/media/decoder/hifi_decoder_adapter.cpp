#include "media/decoder/hifi_decoder_adapter.h"

#include "media/decoder/esp_audio_codec_backend.h"

namespace media {

AudioDecodeStatus HifiDecoderAdapter::open(AudioStream& stream) noexcept {
    if (backend_ == nullptr || !stream.is_open()) {
        opened_ = false;
        error_ = AudioDecodeStatus::InvalidArgument;
        return error_;
    }
    const auto result = backend_->open(stream);
    opened_ = result == AudioDecodeStatus::Ok;
    error_ = result;
    return result;
}

AudioDecodeStatus HifiDecoderAdapter::decode(PcmBlock& block) noexcept {
    if (!opened_ || backend_ == nullptr) {
        error_ = AudioDecodeStatus::NotOpen;
        return error_;
    }
    if (!block.valid()) {
        error_ = AudioDecodeStatus::InvalidArgument;
        return error_;
    }
    const auto result = backend_->decode(block);
    if (result == AudioDecodeStatus::Malformed || result == AudioDecodeStatus::IoError ||
        result == AudioDecodeStatus::Unsupported) {
        error_ = result;
    }
    return result;
}

const PcmFormat& HifiDecoderAdapter::format() const noexcept {
    static const PcmFormat empty{};
    return backend_ != nullptr ? backend_->format() : empty;
}

const AudioMetadata& HifiDecoderAdapter::metadata() const noexcept {
    static const AudioMetadata empty{};
    return backend_ != nullptr ? backend_->metadata() : empty;
}

bool HifiDecoderAdapter::eof() const noexcept {
    return opened_ && backend_ != nullptr && backend_->eof();
}

AudioDecodeStatus HifiDecoderAdapter::last_error() const noexcept {
    if (backend_ == nullptr) return error_;
    return error_ == AudioDecodeStatus::NotOpen ? backend_->last_error() : error_;
}

AudioDecodeStatus HifiDecoderAdapter::reset() noexcept {
    if (backend_ == nullptr) {
        error_ = AudioDecodeStatus::NotOpen;
        return error_;
    }
    const auto result = backend_->reset();
    if (result == AudioDecodeStatus::Ok) {
        // The backend re-opened in place, so a decode() may follow immediately.
        opened_ = true;
        error_ = AudioDecodeStatus::Ok;
    } else {
        error_ = result;
    }
    return result;
}

std::unique_ptr<HifiDecoderBackend> create_hifi_decoder_backend() noexcept {
    // The ESP-IDF decoder owns no I2S or volume state and is safe to use with
    // StackChan's existing AudioSink.  Arduino Audio remains deliberately
    // unsupported here because it would create a second audio owner.
    return create_esp_audio_codec_backend();
}

}  // namespace media
