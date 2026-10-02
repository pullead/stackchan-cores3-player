#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace media {

// Turns PCM into the column levels the dot-matrix spectrum draws.
//
// Runs on the consumer side of PcmTap, never in the audio writer: the decoder
// must not wait on an FFT.  Levels decay rather than snapping to zero, which is
// what makes the display read as a VU meter instead of flickering noise.
class SpectrumAnalyzer {
public:
    // 128 real samples give 64 bins; at 44.1 kHz that is ~345 Hz per bin,
    // enough resolution for a 28-column log-grouped display.
    static constexpr std::size_t kFftSize = 128;
    static constexpr std::size_t kBins = kFftSize / 2;
    static constexpr std::size_t kColumns = 28;

    // Feeds interleaved PCM; extra channels are averaged down first.  Returns
    // true once a full window was consumed and the levels were updated.
    bool push(const int16_t* pcm, std::size_t samples, uint8_t channels) noexcept;

    // Column levels, 0..255, left (bass) to right (treble).
    const std::array<uint8_t, kColumns>& columns() const noexcept { return columns_; }
    // Peak level of the last window, 0..255 -- the VU reading.
    uint8_t level() const noexcept { return level_; }

    // Decays every column and the level one step, for ticks with no new audio.
    void decay() noexcept;
    void reset() noexcept;

private:
    void analyse_window() noexcept;

    std::array<float, kFftSize> window_{};
    std::array<uint8_t, kColumns> columns_{};
    std::size_t filled_ = 0;
    uint8_t level_ = 0;
};

}  // namespace media
