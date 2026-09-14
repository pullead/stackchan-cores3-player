#include "media/decoder/hifi_decoder_adapter.h"

namespace media {

AudioDecodeStatus HifiDecoderAdapter::open(AudioStream& stream) noexcept {
    if (!stream.is_open()) {
        opened_ = false;
        error_ = AudioDecodeStatus::InvalidArgument;
        return error_;
    }
    const auto result = backend_.open(stream);
    opened_ = result == AudioDecodeStatus::Ok;
    error_ = result;
    return result;
}

AudioDecodeStatus HifiDecoderAdapter::decode(PcmBlock& block) noexcept {
    if (!opened_) {
        error_ = AudioDecodeStatus::NotOpen;
        return error_;
    }
    if (!block.valid()) {
        error_ = AudioDecodeStatus::InvalidArgument;
        return error_;
    }
    const auto result = backend_.decode(block);
    if (result == AudioDecodeStatus::Malformed || result == AudioDecodeStatus::IoError ||
        result == AudioDecodeStatus::Unsupported) {
        error_ = result;
    }
    return result;
}

const PcmFormat& HifiDecoderAdapter::format() const noexcept { return backend_.format(); }
const AudioMetadata& HifiDecoderAdapter::metadata() const noexcept { return backend_.metadata(); }
bool HifiDecoderAdapter::eof() const noexcept { return opened_ && backend_.eof(); }
AudioDecodeStatus HifiDecoderAdapter::last_error() const noexcept {
    return error_ == AudioDecodeStatus::NotOpen ? backend_.last_error() : error_;
}

HifiDecoderBackend* create_hifi_decoder_backend() noexcept {
#if defined(CONFIG_STACKCHAN_HIFI_AUDIOI2S_BACKEND)
    // The concrete ESP32-audioI2S bridge is supplied by the optional component
    // and must implement HifiDecoderBackend. It is intentionally not linked
    // into the default StackChan build because that library owns I2S directly.
    extern HifiDecoderBackend* stackchan_create_audioi2s_backend() noexcept;
    return stackchan_create_audioi2s_backend();
#else
    return nullptr;
#endif
}

}  // namespace media
