#include "media/storage/byte_ring.h"

#include <algorithm>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace media {
namespace {

std::size_t round_up_to_power_of_two(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

}  // namespace

ByteRing::~ByteRing() {
    if (buffer_ == nullptr) return;
#ifdef ESP_PLATFORM
    heap_caps_free(buffer_);
#else
    delete[] buffer_;
#endif
}

bool ByteRing::allocate(std::size_t capacity_bytes) {
    if (buffer_ != nullptr || capacity_bytes == 0) return false;
    const std::size_t capacity = round_up_to_power_of_two(capacity_bytes);
#ifdef ESP_PLATFORM
    // Compressed audio is streamed through once, so PSRAM is the right home for
    // a buffer this size; only the per-frame PCM buffers need internal RAM.
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
    if (buffer_ == nullptr) {
        buffer_ = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_8BIT));
    }
#else
    buffer_ = new (std::nothrow) uint8_t[capacity];
#endif
    if (buffer_ == nullptr) return false;
    capacity_ = capacity;
    mask_ = capacity - 1;
    reset();
    return true;
}

std::size_t ByteRing::write(const uint8_t* source, std::size_t count) noexcept {
    if (buffer_ == nullptr || source == nullptr || count == 0) return 0;
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t writable = std::min(count, capacity_ - (head - tail));
    if (writable == 0) return 0;

    const std::size_t offset = head & mask_;
    const std::size_t first = std::min(writable, capacity_ - offset);
    std::memcpy(buffer_ + offset, source, first);
    if (writable > first) {
        std::memcpy(buffer_, source + first, writable - first);
    }
    head_.store(head + writable, std::memory_order_release);
    return writable;
}

std::size_t ByteRing::read(uint8_t* destination, std::size_t count) noexcept {
    if (buffer_ == nullptr || destination == nullptr || count == 0) return 0;
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t readable = std::min(count, head - tail);
    if (readable == 0) return 0;

    const std::size_t offset = tail & mask_;
    const std::size_t first = std::min(readable, capacity_ - offset);
    std::memcpy(destination, buffer_ + offset, first);
    if (readable > first) {
        std::memcpy(destination + first, buffer_, readable - first);
    }
    tail_.store(tail + readable, std::memory_order_release);
    return readable;
}

}  // namespace media
