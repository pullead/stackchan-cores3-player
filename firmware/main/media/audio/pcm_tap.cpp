#include "media/audio/pcm_tap.h"

#include <algorithm>

namespace media {

size_t PcmTap::push(const int16_t* samples, size_t count) noexcept {
    if (!samples || count == 0) return 0;
    const size_t accepted = std::min(count, kCapacity - size_);
    for (size_t i = 0; i < accepted; ++i) {
        buffer_[head_] = samples[i];
        head_ = (head_ + 1) % kCapacity;
    }
    size_ += accepted;
    dropped_ += count - accepted;
    return accepted;
}

size_t PcmTap::pop(int16_t* samples, size_t capacity) noexcept {
    if (!samples || capacity == 0) return 0;
    const size_t count = std::min(capacity, size_);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = buffer_[tail_];
        tail_ = (tail_ + 1) % kCapacity;
    }
    size_ -= count;
    return count;
}

}  // namespace media
