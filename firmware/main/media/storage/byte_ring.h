#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace media {

// Single-producer / single-consumer byte ring.
//
// The producer is the SD prefetch task, the consumer is the audio task.  Reads
// and writes never block each other: the decoder keeps working out of RAM while
// the bus is being borrowed, which is what stops the SD pause from starving the
// I2S DMA.
class ByteRing {
public:
    ByteRing() = default;
    ~ByteRing();

    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;

    // Capacity is rounded to a power of two so index wrapping is a mask.
    bool allocate(std::size_t capacity_bytes);

    std::size_t write(const uint8_t* source, std::size_t count) noexcept;
    std::size_t read(uint8_t* destination, std::size_t count) noexcept;

    std::size_t available() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    std::size_t free_space() const noexcept { return capacity_ - available(); }
    std::size_t capacity() const noexcept { return capacity_; }
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    uint8_t* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    // Monotonic counters; the difference is the fill level.
    std::atomic<std::size_t> head_{0};  // producer owned
    std::atomic<std::size_t> tail_{0};  // consumer owned
};

}  // namespace media
