#pragma once

#include "media/decoder/audio_stream.h"

#include "hal/board/spi3_display_handoff.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace media {

// File callbacks deliberately expose no write operation. The owner keeps the
// SPI3 display handoff acquired for the entire lifetime of the open file.
struct SdAudioFileOperations {
    void* context = nullptr;
    bool (*mount)(void*, std::string&) = nullptr;
    bool (*open)(void*, std::string_view, void*& handle, uint64_t& size, std::string&) = nullptr;
    std::size_t (*read)(void*, void*, uint8_t*, std::size_t, bool&, std::string&) = nullptr;
    bool (*seek)(void*, void*, uint64_t, std::string&) = nullptr;
    bool (*close)(void*, void*, std::string&) = nullptr;
    bool (*unmount)(void*, std::string&) = nullptr;
};

class SdAudioStream final : public AudioStream {
public:
    SdAudioStream(board::Spi3DisplayHandoff& handoff,
                  SdAudioFileOperations operations,
                  std::string path) noexcept;
    ~SdAudioStream() override;

    AudioStreamStatus read(uint8_t* destination, std::size_t capacity,
                           std::size_t& bytes_read) noexcept override;
    AudioStreamStatus seek(uint64_t offset) noexcept override;
    uint64_t tell() const noexcept override { return position_; }
    uint64_t size() const noexcept override { return open_ ? size_ : 0; }
    bool is_open() const noexcept override { return open_; }
    AudioStreamStatus close() noexcept override;
    const std::string& last_error() const noexcept { return last_error_; }

private:
    board::Spi3DisplayHandoff* handoff_;
    SdAudioFileOperations operations_;
    std::unique_ptr<board::Spi3DisplayHandoffGuard> handoff_guard_;
    std::string path_;
    void* file_ = nullptr;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
    bool mounted_ = false;
    bool open_ = false;
    std::string last_error_;
};

}  // namespace media
