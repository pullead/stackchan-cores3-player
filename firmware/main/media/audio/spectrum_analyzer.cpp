#include "media/audio/spectrum_analyzer.h"

#include <algorithm>
#include <cmath>

namespace media {
namespace {

// Columns are grouped logarithmically:音乐的能量集中在低频，线性分组会让右侧
// 大半个屏幕常年不动.  Bin ranges are precomputed once for kBins -> kColumns.
struct ColumnRange {
    std::size_t first;
    std::size_t last;
};

std::array<ColumnRange, SpectrumAnalyzer::kColumns> build_ranges() noexcept {
    std::array<ColumnRange, SpectrumAnalyzer::kColumns> ranges{};
    // Skip bin 0 (DC) and spread the rest over the columns on a log curve.
    const float bins = static_cast<float>(SpectrumAnalyzer::kBins - 1);
    std::size_t previous = 1;
    for (std::size_t c = 0; c < SpectrumAnalyzer::kColumns; ++c) {
        const float fraction = static_cast<float>(c + 1) / SpectrumAnalyzer::kColumns;
        auto edge = static_cast<std::size_t>(std::pow(bins, fraction));
        edge = std::clamp<std::size_t>(edge, previous, SpectrumAnalyzer::kBins - 1);
        ranges[c] = {previous, edge};
        previous = edge + 1 > SpectrumAnalyzer::kBins - 1 ? SpectrumAnalyzer::kBins - 1 : edge + 1;
    }
    return ranges;
}

// Decay per window.  Fast enough to follow a beat, slow enough that the grid
// does not strobe.
constexpr uint8_t kDecayStep = 12;

// Hann window, built once.  analyse_window() runs several hundred times a
// second at 44.1 kHz stereo, and recomputing 128 cosines per window was pure
// UI-thread cost for a table that never changes.
const std::array<float, SpectrumAnalyzer::kFftSize>& hann_window() noexcept {
    static const std::array<float, SpectrumAnalyzer::kFftSize> window = [] {
        std::array<float, SpectrumAnalyzer::kFftSize> values{};
        for (std::size_t i = 0; i < SpectrumAnalyzer::kFftSize; ++i) {
            values[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * i /
                                                 (SpectrumAnalyzer::kFftSize - 1)));
        }
        return values;
    }();
    return window;
}

}  // namespace

bool SpectrumAnalyzer::push(const int16_t* pcm, std::size_t samples, uint8_t channels) noexcept {
    if (pcm == nullptr || samples == 0 || channels == 0) {
        return false;
    }

    bool produced = false;
    const std::size_t frames = samples / channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t sum = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            sum += pcm[frame * channels + channel];
        }
        window_[filled_++] = static_cast<float>(sum) / (channels * 32768.0f);
        if (filled_ == kFftSize) {
            analyse_window();
            filled_ = 0;
            produced = true;
        }
    }
    return produced;
}

void SpectrumAnalyzer::analyse_window() noexcept {
    // In-place iterative radix-2 FFT on a real input; the imaginary half starts
    // at zero.  A Hann window keeps a steady tone from smearing across bins.
    std::array<float, kFftSize> real{};
    std::array<float, kFftSize> imag{};
    const auto& hann = hann_window();
    for (std::size_t i = 0; i < kFftSize; ++i) {
        real[i] = window_[i] * hann[i];
    }

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < kFftSize; ++i) {
        std::size_t bit = kFftSize >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (std::size_t len = 2; len <= kFftSize; len <<= 1) {
        const float angle = -2.0f * 3.14159265358979f / static_cast<float>(len);
        const float w_real = std::cos(angle);
        const float w_imag = std::sin(angle);
        for (std::size_t i = 0; i < kFftSize; i += len) {
            float cur_real = 1.0f;
            float cur_imag = 0.0f;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const float u_real = real[i + k];
                const float u_imag = imag[i + k];
                const float v_real = real[i + k + len / 2] * cur_real - imag[i + k + len / 2] * cur_imag;
                const float v_imag = real[i + k + len / 2] * cur_imag + imag[i + k + len / 2] * cur_real;
                real[i + k] = u_real + v_real;
                imag[i + k] = u_imag + v_imag;
                real[i + k + len / 2] = u_real - v_real;
                imag[i + k + len / 2] = u_imag - v_imag;
                const float next_real = cur_real * w_real - cur_imag * w_imag;
                cur_imag = cur_real * w_imag + cur_imag * w_real;
                cur_real = next_real;
            }
        }
    }

    static const auto ranges = build_ranges();
    uint8_t peak = 0;
    for (std::size_t c = 0; c < kColumns; ++c) {
        float magnitude = 0.0f;
        for (std::size_t bin = ranges[c].first; bin <= ranges[c].last && bin < kBins; ++bin) {
            magnitude = std::max(magnitude, std::hypot(real[bin], imag[bin]));
        }
        // Log scale: linear magnitudes put everything in the bottom row or two.
        const float scaled = magnitude > 0.0f ? 20.0f * std::log10(magnitude) : -80.0f;
        const float normalised = std::clamp((scaled + 60.0f) / 60.0f, 0.0f, 1.0f);
        const auto value = static_cast<uint8_t>(normalised * 255.0f);
        // Rise immediately, fall gradually.
        columns_[c] = value > columns_[c] ? value
                                          : static_cast<uint8_t>(columns_[c] - std::min<uint8_t>(
                                                                     kDecayStep, columns_[c]));
        peak = std::max(peak, columns_[c]);
    }
    level_ = peak;
}

void SpectrumAnalyzer::decay() noexcept {
    uint8_t peak = 0;
    for (auto& column : columns_) {
        column = static_cast<uint8_t>(column - std::min<uint8_t>(kDecayStep, column));
        peak = std::max(peak, column);
    }
    level_ = peak;
}

void SpectrumAnalyzer::reset() noexcept {
    columns_.fill(0);
    window_.fill(0.0f);
    filled_ = 0;
    level_ = 0;
}

}  // namespace media
