#pragma once

#include "media/media_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

class WavReader {
public:
    // The caller must keep bytes valid until the next open(), reset, or this reader is destroyed.
    bool open(const uint8_t* bytes, size_t size) noexcept;
    bool open(const std::vector<uint8_t>& bytes);

    const PcmFormat& format() const noexcept;
    size_t data_offset() const noexcept;
    size_t remaining_frames() const noexcept;
    size_t read_frames(int16_t* destination, size_t max_frames) noexcept;

private:
    bool open_bytes(const uint8_t* bytes, size_t size) noexcept;
    void reset() noexcept;

    std::vector<uint8_t> owned_bytes_;
    const uint8_t* bytes_ = nullptr;
    size_t data_offset_ = 0;
    size_t data_end_ = 0;
    size_t position_ = 0;
    PcmFormat format_{};
    bool open_ = false;
};

}  // namespace media
