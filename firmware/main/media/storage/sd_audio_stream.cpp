#include "media/storage/sd_audio_stream.h"
#include <utility>

namespace media {

SdAudioStream::SdAudioStream(board::Spi3DisplayHandoff& handoff,
                             SdAudioFileOperations operations,
                             std::string path) noexcept
    : handoff_(&handoff), operations_(operations), path_(std::move(path)) {
    if (!operations_.mount || !operations_.open || !operations_.read || !operations_.seek ||
        !operations_.close || !operations_.unmount) { last_error_ = "SD stream callbacks are incomplete"; return; }
    handoff_guard_ = std::make_unique<board::Spi3DisplayHandoffGuard>(*handoff_);
    if (!handoff_guard_->acquired()) { last_error_ = handoff_->last_error(); handoff_guard_.reset(); return; }
    std::string error;
    if (!operations_.mount(operations_.context, error)) { last_error_ = error; close(); return; }
    mounted_ = true;
    if (!operations_.open(operations_.context, path_, file_, size_, error) || !file_) { last_error_ = error; close(); return; }
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
    bool io_error = false;
    bytes_read = operations_.read(operations_.context, file_, destination, capacity, io_error, error);
    position_ += bytes_read;
    if (io_error) { last_error_ = error.empty() ? "SD read failed" : error; return AudioStreamStatus::IoError; }
    if (bytes_read == 0) return position_ >= size_ ? AudioStreamStatus::Eof : (last_error_ = "SD read failed", AudioStreamStatus::IoError);
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::seek(uint64_t offset) noexcept {
    if (!open_) return AudioStreamStatus::Closed;
    if (offset > size_) return AudioStreamStatus::OutOfRange;
    std::string error;
    if (!operations_.seek(operations_.context, file_, offset, error)) { last_error_ = error; return AudioStreamStatus::IoError; }
    position_ = offset;
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::close() noexcept {
    AudioStreamStatus result = AudioStreamStatus::Ok;
    if (file_ != nullptr) { std::string error; if (!operations_.close(operations_.context, file_, error)) { last_error_ = error; result = AudioStreamStatus::IoError; } file_ = nullptr; }
    open_ = false;
    position_ = 0;
    size_ = 0;
    if (mounted_) { std::string error; if (!operations_.unmount(operations_.context, error)) { last_error_ = error; result = AudioStreamStatus::IoError; } mounted_ = false; }
    handoff_guard_.reset();
    return result;
}

}  // namespace media
