#include "media/storage/prefetching_stream.h"

#include <algorithm>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_log.h>

#define TAG "Prefetch"
#endif

namespace media {

PrefetchingStream::PrefetchingStream(std::unique_ptr<AudioStream> source, std::size_t ring_bytes,
                                     std::size_t chunk_bytes) noexcept
    : source_(std::move(source)), chunk_bytes_(chunk_bytes) {
    if (!source_ || !source_->is_open() || chunk_bytes_ == 0) {
        failed_.store(true, std::memory_order_release);
        return;
    }
    if (!ring_.allocate(ring_bytes)) {
        failed_.store(true, std::memory_order_release);
        return;
    }

#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCore(trampoline, "sd_prefetch", kStackBytes, this,
                                                       kPriority, &handle_, kCoreId);
    if (created != pdPASS) {
        handle_ = nullptr;
        ESP_LOGE(TAG, "Could not create the SD prefetch task");
        failed_.store(true, std::memory_order_release);
    }
#endif
}

PrefetchingStream::~PrefetchingStream() { close(); }

bool PrefetchingStream::fill_once() noexcept {
    if (!source_ || source_eof_.load(std::memory_order_acquire)) {
        return false;
    }
    if (ring_.free_space() < chunk_bytes_) {
        return false;
    }

    std::vector<uint8_t> chunk(chunk_bytes_);
    std::size_t read = 0;
    const AudioStreamStatus status = source_->read(chunk.data(), chunk_bytes_, read);
    if (read > 0) {
        // Space was checked above, so the whole chunk fits.
        ring_.write(chunk.data(), read);
    }
    if (status == AudioStreamStatus::Eof) {
        source_eof_.store(true, std::memory_order_release);
        return read > 0;
    }
    if (status != AudioStreamStatus::Ok) {
        failed_.store(true, std::memory_order_release);
        source_eof_.store(true, std::memory_order_release);
        return false;
    }
    return read > 0;
}

AudioStreamStatus PrefetchingStream::read(uint8_t* destination, std::size_t capacity,
                                          std::size_t& bytes_read) noexcept {
    bytes_read = 0;
    if (!source_) return AudioStreamStatus::Closed;
    if (destination == nullptr && capacity != 0) return AudioStreamStatus::InvalidArgument;
    if (capacity == 0) return AudioStreamStatus::Ok;

    while (bytes_read < capacity) {
        const std::size_t taken = ring_.read(destination + bytes_read, capacity - bytes_read);
        bytes_read += taken;
        position_ += taken;
        if (bytes_read == capacity) break;

        if (source_eof_.load(std::memory_order_acquire) && ring_.available() == 0) {
            break;
        }
        if (taken == 0) {
            // The fill task has not caught up.  Count it: a healthy stream
            // never gets here, and this is the signal that prefetching is
            // undersized rather than a silent stutter.
            ++starve_count_;
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#else
            if (!fill_once()) break;
#endif
        }
    }

    if (bytes_read > 0) return AudioStreamStatus::Ok;
    if (failed_.load(std::memory_order_acquire)) return AudioStreamStatus::IoError;
    return AudioStreamStatus::Eof;
}

AudioStreamStatus PrefetchingStream::seek(uint64_t offset) noexcept {
    if (!source_) return AudioStreamStatus::Closed;
    // Seeking invalidates everything read ahead.  Stop the filler first so it
    // cannot append data from the old position after the ring is cleared.
    stop_task();
    const AudioStreamStatus status = source_->seek(offset);
    ring_.reset();
    position_ = offset;
    source_eof_.store(false, std::memory_order_release);
    return status;
}

void PrefetchingStream::stop_task() noexcept {
#ifdef ESP_PLATFORM
    if (handle_ == nullptr && !running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    handle_ = nullptr;
    stop_requested_.store(false, std::memory_order_release);
#endif
}

AudioStreamStatus PrefetchingStream::close() noexcept {
    // The filler touches the source, so it must be stopped before the source is
    // closed, not after.
    stop_task();
    if (!source_) return AudioStreamStatus::Ok;
    const AudioStreamStatus status = source_->close();
    source_.reset();
    return status;
}

#ifdef ESP_PLATFORM

void PrefetchingStream::trampoline(void* argument) noexcept {
    auto* stream = static_cast<PrefetchingStream*>(argument);
    stream->run();
    stream->running_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void PrefetchingStream::run() noexcept {
    running_.store(true, std::memory_order_release);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (!fill_once()) {
            // Either the ring is full or the source is done; either way there
            // is nothing useful to do right now.
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

#endif

}  // namespace media
