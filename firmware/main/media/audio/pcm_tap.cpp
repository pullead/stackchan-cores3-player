#include "media/audio/pcm_tap.h"

#include <algorithm>

namespace media {

size_t PcmTap::push(const int16_t* samples, size_t count) noexcept {
    if (!samples || count == 0) return 0;
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t accepted = std::min(count, kCapacity - (head - tail));
    for (size_t i = 0; i < accepted; ++i) {
        buffer_[(head + i) % kCapacity] = samples[i];
    }
    head_.store(head + accepted, std::memory_order_release);
    dropped_.fetch_add(count - accepted, std::memory_order_relaxed);
    return accepted;
}

size_t PcmTap::pop(int16_t* samples, size_t capacity) noexcept {
    if (!samples || capacity == 0) return 0;
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t count = std::min(capacity, head - tail);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = buffer_[(tail + i) % kCapacity];
    }
    tail_.store(tail + count, std::memory_order_release);
    return count;
}

}  // namespace media
