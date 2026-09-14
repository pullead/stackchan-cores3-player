#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

enum class AudioStreamStatus : uint8_t {
    Ok,
    Eof,
    InvalidArgument,
    OutOfRange,
    IoError,
    Closed,
};

// Read-only byte stream consumed by a decoder. Implementations own their
// storage; callers only provide a destination buffer and never write through
// this interface.
class AudioStream {
public:
    virtual ~AudioStream() = default;

    virtual AudioStreamStatus read(uint8_t* destination,
                                   size_t capacity,
                                   size_t& bytes_read) noexcept = 0;
    virtual AudioStreamStatus seek(uint64_t offset) noexcept = 0;
    virtual uint64_t tell() const noexcept = 0;
    virtual uint64_t size() const noexcept = 0;
    virtual AudioStreamStatus close() noexcept = 0;
};

}  // namespace media
