#include "media/audio/spectrum_analyzer.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "check failed: %s\n", what);
    return condition;
}

// Generates one window's worth of a mono sine at the given fraction of the
// sample rate (0.5 = Nyquist).
std::vector<int16_t> sine(double fraction, std::size_t samples, double amplitude = 0.8) {
    std::vector<int16_t> pcm(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        pcm[i] = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * fraction * i) * amplitude *
                                      32767.0);
    }
    return pcm;
}

// Index of the loudest column.
std::size_t loudest(const media::SpectrumAnalyzer& analyzer) {
    std::size_t index = 0;
    for (std::size_t i = 1; i < media::SpectrumAnalyzer::kColumns; ++i) {
        if (analyzer.columns()[i] > analyzer.columns()[index]) index = i;
    }
    return index;
}

bool test_a_full_window_is_needed_before_levels_appear() {
    media::SpectrumAnalyzer analyzer;
    const auto pcm = sine(0.1, media::SpectrumAnalyzer::kFftSize / 2);
    return check(!analyzer.push(pcm.data(), pcm.size(), 1), "half a window produces nothing") &&
           check(analyzer.level() == 0, "no level before the first window");
}

bool test_a_tone_lights_a_column() {
    media::SpectrumAnalyzer analyzer;
    const auto pcm = sine(0.1, media::SpectrumAnalyzer::kFftSize);
    return check(analyzer.push(pcm.data(), pcm.size(), 1), "a full window is analysed") &&
           check(analyzer.level() > 0, "a tone produces a level");
}

bool test_pitch_maps_to_position() {
    // A low tone must light a column left of a high tone's.  The exact column
    // depends on the log grouping, so the test pins the ordering, not indices.
    media::SpectrumAnalyzer low_analyzer;
    const auto low = sine(0.02, media::SpectrumAnalyzer::kFftSize);
    low_analyzer.push(low.data(), low.size(), 1);

    media::SpectrumAnalyzer high_analyzer;
    const auto high = sine(0.40, media::SpectrumAnalyzer::kFftSize);
    high_analyzer.push(high.data(), high.size(), 1);

    return check(loudest(low_analyzer) < loudest(high_analyzer),
                 "a lower tone peaks further left than a higher one");
}

bool test_silence_decays_instead_of_snapping() {
    media::SpectrumAnalyzer analyzer;
    const auto pcm = sine(0.1, media::SpectrumAnalyzer::kFftSize);
    analyzer.push(pcm.data(), pcm.size(), 1);
    const uint8_t peak = analyzer.level();
    if (!check(peak > 0, "a tone produces a level")) return false;

    analyzer.decay();
    const uint8_t after = analyzer.level();
    return check(after < peak, "the level falls after the tone stops") &&
           check(after > 0, "one decay step does not drop straight to zero");
}

bool test_stereo_is_averaged_not_misread() {
    // Interleaved stereo must be folded to mono, otherwise the window would be
    // filled with alternating channels and read as a spurious high frequency.
    media::SpectrumAnalyzer mono_analyzer;
    const auto mono = sine(0.05, media::SpectrumAnalyzer::kFftSize);
    mono_analyzer.push(mono.data(), mono.size(), 1);

    std::vector<int16_t> stereo(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
    media::SpectrumAnalyzer stereo_analyzer;
    stereo_analyzer.push(stereo.data(), stereo.size(), 2);

    return check(loudest(stereo_analyzer) == loudest(mono_analyzer),
                 "the same tone peaks in the same column in mono and stereo");
}

bool test_reset_clears_everything() {
    media::SpectrumAnalyzer analyzer;
    const auto pcm = sine(0.1, media::SpectrumAnalyzer::kFftSize);
    analyzer.push(pcm.data(), pcm.size(), 1);
    analyzer.reset();
    return check(analyzer.level() == 0, "reset clears the level") &&
           check(analyzer.columns()[loudest(analyzer)] == 0, "reset clears the columns");
}

}  // namespace

int main() {
    const bool ok = test_a_full_window_is_needed_before_levels_appear() &&
                    test_a_tone_lights_a_column() && test_pitch_maps_to_position() &&
                    test_silence_decays_instead_of_snapping() &&
                    test_stereo_is_averaged_not_misread() && test_reset_clears_everything();
    return ok ? 0 : 1;
}
