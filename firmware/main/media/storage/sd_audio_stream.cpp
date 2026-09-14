#include "media/storage/sd_audio_stream.h"

namespace media {

SdAudioStream::SdAudioStream(board::Spi3DisplayHandoff& handoff,
                             SdAudioFileOperations operations,
                             std::string path) noexcept
    : handoff_(&handoff), operations_(operations), path_(std::move(path)) {
    if (!operations_.mount || !operations_.open || !operations_.read || !operations_.seek ||
        !operations_.close || !operations_.unmount) return;
    handoff_guard_ = std::make_unique<board::Spi3DisplayHandoffGuard>(*handoff_);
    if (!handoff_guard_->acquired()) { handoff_guard_.reset(); return; }
    std::string error;
    if (!operations_.mount(operations_.context, error)) { close(); return; }
    mounted_ = true;
    if (!operations_.open(operations_.context, path_, file_, size_, error) || !file_) { close(); return; }
    open_ = true;
}

SdAudioStream::~SdAudioStream() { close(); }

AudioStreamStatus SdAudioStream::read(uint8_t* destination, std::size_t capacity,
                                      std::size_t& bytes_read) noexcept {
    bytes_read = 0;
    if (!open_) return AudioStreamStatus::Closed;
    if (destination == nullptr && capacity != 0) return AudioStreamStatus::InvalidArgument;
    if (capacity == 0) return AudioStreamStatus::Ok;
    std::string error;
    bytes_read = operations_.read(operations_.context, file_, destination, capacity, error);
    position_ += bytes_read;
    if (bytes_read == 0) return position_ >= size_ ? AudioStreamStatus::Eof : AudioStreamStatus::IoError;
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::seek(uint64_t offset) noexcept {
    if (!open_) return AudioStreamStatus::Closed;
    if (offset > size_) return AudioStreamStatus::OutOfRange;
    std::string error;
    if (!operations_.seek(operations_.context, file_, offset, error)) return AudioStreamStatus::IoError;
    position_ = offset;
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::close() noexcept {
    if (file_ != nullptr) { operations_.close(operations_.context, file_); file_ = nullptr; }
    open_ = false;
    position_ = 0;
    size_ = 0;
    if (mounted_) { std::string error; operations_.unmount(operations_.context, error); mounted_ = false; }
    handoff_guard_.reset();
    return AudioStreamStatus::Ok;
}

}  // namespace media
