#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace media {

// A bounded, non-blocking PCM observation point. The intended use is one
// producer (the audio writer) and one consumer (a future FFT task). The
// current API is called from the same media task; a concurrent integration
// must provide SPSC scheduling/ordering around push/pop. The writer never
// waits for a consumer; overflow is observable.
class PcmTap {
public:
    // One decoded chunk is a full kPlaybackChunkFrames window: up to 4096
    // interleaved samples in stereo.  The producer only checks for space once
    // per push, so a ring smaller than one chunk truncates every single chunk.
    // Two chunks of room let a UI frame that arrives late drain what it missed
    // instead of losing half of it.
    static constexpr size_t kCapacity = 8192;
    size_t push(const int16_t* samples, size_t count) noexcept;
    size_t pop(int16_t* samples, size_t capacity) noexcept;
    size_t available() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    void reset() noexcept { tail_.store(0); head_.store(0); dropped_.store(0); }

private:
    int16_t buffer_[kCapacity]{};
    std::atomic<size_t> head_{0}; // producer-owned monotonic counter
    std::atomic<size_t> tail_{0}; // consumer-owned monotonic counter
    std::atomic<size_t> dropped_{0};
};

}  // namespace media
