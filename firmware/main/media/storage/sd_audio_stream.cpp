#include "media/storage/sd_audio_stream.h"

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef ESP_PLATFORM
#include <esp_log.h>

#define TAG "SdAudioStream"
#endif

namespace media {

SdAudioStream::SdAudioStream(board::Spi3DisplayHandoff& handoff,
                             SdAudioFileOperations operations,
                             std::string path,
                             std::size_t prefetch_bytes) noexcept
    : handoff_(&handoff), operations_(operations), path_(std::move(path)) {
    if (!operations_.mount || !operations_.open || !operations_.read || !operations_.seek ||
        !operations_.close || !operations_.unmount) {
        last_error_ = "SD stream callbacks are incomplete";
        return;
    }
    if (prefetch_bytes == 0) {
        last_error_ = "SD stream prefetch size must be positive";
        return;
    }

    // Mounting and opening borrow the bus, then hand it straight back so the
    // display keeps running while the track plays.
    board::Spi3DisplayHandoffGuard guard(*handoff_);
    if (!guard.acquired()) {
        last_error_ = handoff_->last_error();
        return;
    }
    ++borrow_count_;

    std::string error;
    if (!operations_.mount(operations_.context, error)) {
        last_error_ = error;
        return;
    }
    mounted_ = true;
    if (!operations_.open(operations_.context, path_, file_, size_, error) || !file_) {
        last_error_ = error;
        std::string unmount_error;
        if (operations_.unmount(operations_.context, unmount_error)) {
            mounted_ = false;
        }
        file_ = nullptr;
        return;
    }

    prefetch_.assign(prefetch_bytes, 0);
    open_ = true;
}

SdAudioStream::~SdAudioStream() { close(); }

AudioStreamStatus SdAudioStream::refill() noexcept {
    prefetch_offset_ = 0;
    prefetch_length_ = 0;
    if (source_eof_) {
        return AudioStreamStatus::Eof;
    }

    board::Spi3DisplayHandoffGuard guard(*handoff_);
    if (!guard.acquired()) {
        last_error_ = handoff_->last_error();
        return AudioStreamStatus::IoError;
    }
    ++borrow_count_;

    std::string error;
    bool io_error = false;
    const std::size_t count = operations_.read(operations_.context, file_, prefetch_.data(),
                                               prefetch_.size(), io_error, error);
    if (io_error) {
        last_error_ = error.empty() ? "SD read failed" : error;
        return AudioStreamStatus::IoError;
    }
    if (count == 0) {
        source_eof_ = true;
        // A short file ends cleanly; a zero-length read before the end is a
        // genuine I/O failure, not EOF.
        if (position_ >= size_) {
            return AudioStreamStatus::Eof;
        }
        last_error_ = "SD read failed";
        return AudioStreamStatus::IoError;
    }
    prefetch_length_ = std::min(count, prefetch_.size());
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::read(uint8_t* destination, std::size_t capacity,
                                      std::size_t& bytes_read) noexcept {
    bytes_read = 0;
    if (!open_) return AudioStreamStatus::Closed;
    if (destination == nullptr && capacity != 0) return AudioStreamStatus::InvalidArgument;
    if (capacity == 0) return AudioStreamStatus::Ok;

    while (bytes_read < capacity) {
        if (buffered() == 0) {
            const AudioStreamStatus status = refill();
            if (status == AudioStreamStatus::Eof) {
                // Deliver whatever was already buffered before reporting EOF.
                return bytes_read > 0 ? AudioStreamStatus::Ok : AudioStreamStatus::Eof;
            }
            if (status != AudioStreamStatus::Ok) {
                return bytes_read > 0 ? AudioStreamStatus::Ok : status;
            }
        }

        const std::size_t take = std::min(capacity - bytes_read, buffered());
        std::memcpy(destination + bytes_read, prefetch_.data() + prefetch_offset_, take);
        prefetch_offset_ += take;
        bytes_read += take;
        position_ += take;
    }
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::seek(uint64_t offset) noexcept {
    if (!open_) return AudioStreamStatus::Closed;
    if (offset > size_) return AudioStreamStatus::OutOfRange;

    board::Spi3DisplayHandoffGuard guard(*handoff_);
    if (!guard.acquired()) {
        last_error_ = handoff_->last_error();
        return AudioStreamStatus::IoError;
    }
    ++borrow_count_;

    std::string error;
    if (!operations_.seek(operations_.context, file_, offset, error)) {
        last_error_ = error;
        return AudioStreamStatus::IoError;
    }
    // Buffered bytes belong to the old position and must not be delivered.
    prefetch_offset_ = 0;
    prefetch_length_ = 0;
    source_eof_ = false;
    position_ = offset;
    return AudioStreamStatus::Ok;
}

AudioStreamStatus SdAudioStream::close() noexcept {
    if (file_ == nullptr && !mounted_) {
        open_ = false;
        return AudioStreamStatus::Ok;
    }

    AudioStreamStatus result = AudioStreamStatus::Ok;
    board::Spi3DisplayHandoffGuard guard(*handoff_);
    if (!guard.acquired()) {
        // Without the bus the card cannot be released cleanly; do not pretend
        // it was, but still drop this stream's own state.
        last_error_ = handoff_->last_error();
        result = AudioStreamStatus::IoError;
    } else {
        ++borrow_count_;
        if (file_ != nullptr) {
            std::string error;
            if (!operations_.close(operations_.context, file_, error)) {
                last_error_ = error;
                result = AudioStreamStatus::IoError;
            }
        }
        if (mounted_) {
            std::string error;
            if (!operations_.unmount(operations_.context, error)) {
                last_error_ = error;
                result = AudioStreamStatus::IoError;
            }
            mounted_ = false;
        }
    }

#ifdef ESP_PLATFORM
    // How often the display bus had to be taken away.  Should track prefetches
    // (file size / prefetch size), not decoder reads; a number close to the
    // read count would mean the screen is being blocked continuously.
    ESP_LOGI(TAG, "Closed after %u bus borrows, %u bytes delivered",
             static_cast<unsigned>(borrow_count_), static_cast<unsigned>(position_));
#endif

    file_ = nullptr;
    open_ = false;
    position_ = 0;
    size_ = 0;
    prefetch_offset_ = 0;
    prefetch_length_ = 0;
    source_eof_ = false;
    return result;
}

}  // namespace media
