#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

// A bounded, non-blocking PCM observation point. The writer never waits for
// a consumer; overflow is observable and may be used by a future FFT task.
class PcmTap {
public:
    static constexpr size_t kCapacity = 2048;
    size_t push(const int16_t* samples, size_t count) noexcept;
    size_t pop(int16_t* samples, size_t capacity) noexcept;
    size_t available() const noexcept { return size_; }
    size_t dropped() const noexcept { return dropped_; }
    void reset() noexcept { head_ = tail_ = size_ = dropped_ = 0; }

private:
    int16_t buffer_[kCapacity]{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
    size_t dropped_ = 0;
};

}  // namespace media
