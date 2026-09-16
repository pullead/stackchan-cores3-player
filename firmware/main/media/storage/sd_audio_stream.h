#pragma once

#include "media/decoder/audio_stream.h"

#include "hal/board/spi3_display_handoff.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace media {

// File callbacks deliberately expose no write operation.
struct SdAudioFileOperations {
    void* context = nullptr;
    bool (*mount)(void*, std::string&) = nullptr;
    bool (*open)(void*, std::string_view, void*& handle, uint64_t& size, std::string&) = nullptr;
    std::size_t (*read)(void*, void*, uint8_t*, std::size_t, bool&, std::string&) = nullptr;
    bool (*seek)(void*, void*, uint64_t, std::string&) = nullptr;
    bool (*close)(void*, void*, std::string&) = nullptr;
    bool (*unmount)(void*, std::string&) = nullptr;
};

// Reads one track from the CoreS3 SD card.
//
// GPIO35 is shared between the display's D/C path and SPI3 MISO, so every SD
// access has to borrow the bus through Spi3DisplayHandoff, which also holds the
// LVGL lock.  Holding it for a whole track would freeze the screen for the
// length of the song, so this stream borrows the bus in short bursts instead:
// one large prefetch fills a RAM buffer, the bus is handed straight back, and
// the decoder is then served from RAM until the buffer runs dry.  The card
// stays mounted throughout; only the pin routing and the LVGL lock are cycled.
class SdAudioStream final : public AudioStream {
public:
    // Sized against the I2S DMA depth, not against comfort: the ring holds
    // 6 x 240 frames, i.e. ~33 ms at 44.1 kHz, and the decoder produces nothing
    // while the bus is borrowed.  Reading 64 KB took longer than that and
    // starved the DMA; 16 KB keeps each pause near 10 ms.
    static constexpr std::size_t kPrefetchBytes = 16 * 1024;

    SdAudioStream(board::Spi3DisplayHandoff& handoff,
                  SdAudioFileOperations operations,
                  std::string path,
                  std::size_t prefetch_bytes = kPrefetchBytes) noexcept;
    ~SdAudioStream() override;

    AudioStreamStatus read(uint8_t* destination, std::size_t capacity,
                           std::size_t& bytes_read) noexcept override;
    AudioStreamStatus seek(uint64_t offset) noexcept override;
    uint64_t tell() const noexcept override { return position_; }
    uint64_t size() const noexcept override { return open_ ? size_ : 0; }
    bool is_open() const noexcept override { return open_; }
    AudioStreamStatus close() noexcept override;
    const std::string& last_error() const noexcept { return last_error_; }

    // Diagnostics: how often the display bus had to be taken away.
    std::size_t borrow_count() const noexcept { return borrow_count_; }

private:
    // Refills the prefetch buffer, borrowing the bus for the duration.
    AudioStreamStatus refill() noexcept;
    std::size_t buffered() const noexcept { return prefetch_length_ - prefetch_offset_; }

    board::Spi3DisplayHandoff* handoff_;
    SdAudioFileOperations operations_;
    std::string path_;
    std::vector<uint8_t> prefetch_;
    std::size_t prefetch_offset_ = 0;
    std::size_t prefetch_length_ = 0;
    std::size_t borrow_count_ = 0;
    void* file_ = nullptr;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
    bool mounted_ = false;
    bool open_ = false;
    bool source_eof_ = false;
    std::string last_error_;
};

}  // namespace media
