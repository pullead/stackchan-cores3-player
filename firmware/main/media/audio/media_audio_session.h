#pragma once

#include <cstdint>
#include <string>

namespace media {

// One I2S clock serves both the AI voice path and media playback on CoreS3.
// A format is only usable if the codec can actually be retimed to it.
struct AudioSessionFormat {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;

    bool valid() const noexcept;
    bool operator==(const AudioSessionFormat& other) const noexcept {
        return sample_rate == other.sample_rate && channels == other.channels &&
               bits_per_sample == other.bits_per_sample;
    }
    bool operator!=(const AudioSessionFormat& other) const noexcept { return !(*this == other); }
};

// Minimal control surface of the board audio path.  Deliberately narrow: the
// session must not be able to write PCM, change volume, or touch the display.
class AudioSessionPort {
public:
    virtual ~AudioSessionPort() = default;

    virtual AudioSessionFormat output_format() const noexcept = 0;
    virtual bool input_enabled() const noexcept = 0;
    virtual bool set_input_enabled(bool enabled) noexcept = 0;
    virtual bool reconfigure_output(const AudioSessionFormat& format) noexcept = 0;
};

// Takes the shared audio channel from the AI voice path for media playback and
// hands it back afterwards.
//
// CoreS3 drives the AW88298 speaker and the ES7210 microphones from a single
// duplex I2S0 channel whose TX and RX share one clock, so the AI path's 24 kHz
// mono configuration cannot coexist with 44.1 kHz stereo music.  Media and AI
// are mutually exclusive modes, which makes retiming the channel legitimate:
// the microphone is released first, the output is retimed, and the original AI
// configuration is restored on release.
//
// A failed handover always rolls back.  If the rollback itself fails the
// session reports `degraded()`, meaning the AI path may be unusable until the
// codec is reinitialised -- this is never silently ignored.
class MediaAudioSession {
public:
    explicit MediaAudioSession(AudioSessionPort& port) noexcept : port_(port) {}

    // Mooncake destroys apps without calling onClose(), so destruction is the
    // last line of defence against leaving the AI path on the music clock.
    ~MediaAudioSession() { release(); }

    MediaAudioSession(const MediaAudioSession&) = delete;
    MediaAudioSession& operator=(const MediaAudioSession&) = delete;

    bool acquire(const AudioSessionFormat& desired) noexcept;
    bool release() noexcept;

    bool held() const noexcept { return held_; }
    bool degraded() const noexcept { return degraded_; }
    const AudioSessionFormat& restore_format() const noexcept { return restore_format_; }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    bool fail(std::string error) noexcept;

    AudioSessionPort& port_;
    bool held_ = false;
    bool degraded_ = false;
    bool restore_input_ = false;
    AudioSessionFormat restore_format_{};
    std::string last_error_;
};

}  // namespace media
