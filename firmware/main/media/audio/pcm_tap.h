#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

// A bounded, non-blocking PCM observation point. The intended use is one
// producer (the audio writer) and one consumer (a future FFT task). The
// current API is called from the same media task; a concurrent integration
// must provide SPSC scheduling/ordering around push/pop. The writer never
// waits for a consumer; overflow is observable.
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
