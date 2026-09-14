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
                                   std::size_t capacity,
                                   std::size_t& bytes_read) noexcept = 0;
    virtual AudioStreamStatus seek(uint64_t offset) noexcept = 0;
    virtual uint64_t tell() const noexcept = 0;
    virtual uint64_t size() const noexcept = 0;
    // close() is idempotent and returns Ok on repeated calls. After close(),
    // is_open() is false, read()/seek() return Closed, and tell()/size() are 0.
    virtual bool is_open() const noexcept = 0;
    virtual AudioStreamStatus close() noexcept = 0;
};

}  // namespace media
