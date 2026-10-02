#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "media/decoder/audio_stream.h"
#include "media/storage/byte_ring.h"

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace media {

// Reads ahead from a slower stream on its own task.
//
// Decoding and SD access used to share one task, so the milliseconds spent
// borrowing the display bus produced no PCM and the I2S DMA ran dry -- playback
// settled about 1.6% below the sample rate, which is exactly the share of time
// the SD reads occupied.  Here the fill task keeps a RAM ring topped up while
// the audio task decodes out of it, so a bus borrow no longer stalls decoding.
class PrefetchingStream final : public AudioStream {
public:
    static constexpr std::size_t kRingBytes = 256 * 1024;
    // Refill in chunks that keep each bus borrow short.
    static constexpr std::size_t kChunkBytes = 16 * 1024;
    // Below the audio task (4) so decoding always wins, above the idle work.
    static constexpr unsigned kPriority = 3;
    static constexpr uint32_t kStackBytes = 4096;
    static constexpr int kCoreId = 1;

    PrefetchingStream(std::unique_ptr<AudioStream> source, std::size_t ring_bytes = kRingBytes,
                      std::size_t chunk_bytes = kChunkBytes) noexcept;
    ~PrefetchingStream() override;

    AudioStreamStatus read(uint8_t* destination, std::size_t capacity,
                           std::size_t& bytes_read) noexcept override;
    AudioStreamStatus seek(uint64_t offset) noexcept override;
    uint64_t tell() const noexcept override { return position_; }
    uint64_t size() const noexcept override { return source_ ? source_->size() : 0; }
    bool is_open() const noexcept override { return source_ && source_->is_open(); }
    AudioStreamStatus close() noexcept override;

    // Pumps one refill chunk.  The task calls this in a loop; host tests drive
    // it directly, which is why the filling logic lives outside the task.
    bool fill_once() noexcept;

    bool source_exhausted() const noexcept { return source_eof_.load(std::memory_order_acquire); }
    std::size_t buffered() const noexcept { return ring_.available(); }
    // Times the decoder found the ring empty: a healthy stream never does.
    std::size_t starve_count() const noexcept { return starve_count_; }

private:
    bool start_task() noexcept;
    void stop_task() noexcept;

    // A read waits for the filler rather than failing instantly, but never
    // forever: pump() holds the controller lock while reading, so an unbounded
    // wait freezes every UI action that needs that lock.
    static constexpr unsigned kMaxStarveTicks = 1000;
#ifdef ESP_PLATFORM
    static void trampoline(void* argument) noexcept;
    void run() noexcept;
#endif

    std::unique_ptr<AudioStream> source_;
    ByteRing ring_;
    std::size_t chunk_bytes_;
    // Refill staging, sized on first use.  Allocating it per fill put a 16 KB
    // allocation on the audio path, inside a noexcept function.
    std::vector<uint8_t> chunk_;
    uint64_t position_ = 0;
    std::size_t starve_count_ = 0;
    std::atomic<bool> source_eof_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
#ifdef ESP_PLATFORM
    TaskHandle_t handle_ = nullptr;
#endif
};

}  // namespace media
