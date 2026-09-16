#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace media {

// A PCM buffer that stays in internal RAM.
//
// Anything over 512 bytes is allocated from PSRAM by default on this board,
// and PSRAM is far slower than internal RAM for the per-frame reads and writes
// the decoder and the sink do.  Leaving these buffers in PSRAM costs enough
// throughput to starve the I2S DMA.  Bulk buffers that are only streamed
// through once, such as the SD prefetch, are fine in PSRAM and do not use this.
class AudioBuffer {
public:
    AudioBuffer() = default;

    explicit AudioBuffer(std::size_t samples) { allocate(samples); }

    ~AudioBuffer() { release(); }

    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    bool allocate(std::size_t samples) {
        release();
        if (samples == 0) return true;
#ifdef ESP_PLATFORM
        data_ = static_cast<int16_t*>(
            heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
        data_ = new (std::nothrow) int16_t[samples];
#endif
        if (data_ == nullptr) return false;
        size_ = samples;
        return true;
    }

    int16_t* data() noexcept { return data_; }
    const int16_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool valid() const noexcept { return data_ != nullptr; }

private:
    void release() noexcept {
        if (data_ == nullptr) return;
#ifdef ESP_PLATFORM
        heap_caps_free(data_);
#else
        delete[] data_;
#endif
        data_ = nullptr;
        size_ = 0;
    }

    int16_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace media
