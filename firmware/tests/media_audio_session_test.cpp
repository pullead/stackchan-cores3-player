#include "media/audio/media_audio_session.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using media::AudioSessionFormat;
using media::MediaAudioSession;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", what);
    }
    return condition;
}

enum class Event { DisableInput, EnableInput, Reconfigure };

// Stands in for the CoreS3 codec: the AI voice path owns a 24 kHz duplex
// channel, and media playback has to take the output over and hand it back.
class FakePort final : public media::AudioSessionPort {
public:
    AudioSessionFormat output_format() const noexcept override { return format_; }
    bool input_enabled() const noexcept override { return input_enabled_; }

    bool set_input_enabled(bool enabled) noexcept override {
        events.push_back(enabled ? Event::EnableInput : Event::DisableInput);
        if (enabled && fail_enable_input) return false;
        if (!enabled && fail_disable_input) return false;
        input_enabled_ = enabled;
        return true;
    }

    bool reconfigure_output(const AudioSessionFormat& format) noexcept override {
        events.push_back(Event::Reconfigure);
        reconfigured.push_back(format);
        if (fail_reconfigure_to.sample_rate != 0 && format.sample_rate == fail_reconfigure_to.sample_rate) {
            return false;
        }
        format_ = format;
        return true;
    }

    std::vector<Event> events;
    std::vector<AudioSessionFormat> reconfigured;
    bool fail_disable_input = false;
    bool fail_enable_input = false;
    AudioSessionFormat fail_reconfigure_to{};

private:
    AudioSessionFormat format_{24000, 1, 16};
    bool input_enabled_ = true;
};

const AudioSessionFormat kHiFi{44100, 2, 16};
const AudioSessionFormat kAi{24000, 1, 16};

bool test_format_validation_rejects_unusable_clocks() {
    return check(!AudioSessionFormat{}.valid(), "empty format is invalid") &&
           check(!AudioSessionFormat{44100, 0, 16}.valid(), "zero channels is invalid") &&
           check(!AudioSessionFormat{44100, 3, 16}.valid(), "three channels is invalid") &&
           check(!AudioSessionFormat{44100, 2, 24}.valid(), "24-bit is unsupported by the sink") &&
           check(!AudioSessionFormat{44101, 2, 16}.valid(), "off-grid sample rate is invalid") &&
           check(AudioSessionFormat{44100, 2, 16}.valid(), "44.1k stereo S16 is valid") &&
           check(AudioSessionFormat{48000, 2, 16}.valid(), "48k stereo S16 is valid") &&
           check(kAi.valid(), "the AI duplex format stays valid");
}

bool test_acquire_releases_microphone_before_retiming_output() {
    FakePort port;
    MediaAudioSession session(port);

    if (!check(session.acquire(kHiFi), "acquire succeeds") ||
        !check(session.held(), "session is held after acquire") ||
        !check(!session.degraded(), "a clean acquire is not degraded")) {
        return false;
    }

    // Order matters: the I2S channel cannot be retimed while the shared duplex
    // RX side is still running for the wake word.
    return check(port.events == std::vector<Event>{Event::DisableInput, Event::Reconfigure},
                 "input is disabled before the output is reconfigured") &&
           check(!port.input_enabled(), "microphone stays off during media playback") &&
           check(port.output_format().sample_rate == 44100 && port.output_format().channels == 2,
                 "output runs at the requested media format");
}

bool test_release_restores_the_ai_duplex_channel() {
    FakePort port;
    MediaAudioSession session(port);
    if (!check(session.acquire(kHiFi), "acquire succeeds")) return false;
    port.events.clear();

    if (!check(session.release(), "release succeeds") ||
        !check(!session.held(), "session is not held after release")) {
        return false;
    }

    return check(port.events == std::vector<Event>{Event::Reconfigure, Event::EnableInput},
                 "output is retimed before the microphone comes back") &&
           check(port.output_format().sample_rate == 24000 && port.output_format().channels == 1,
                 "the AI duplex format is restored exactly") &&
           check(port.input_enabled(), "the microphone is handed back to the AI path");
}

bool test_invalid_format_never_touches_the_codec() {
    FakePort port;
    MediaAudioSession session(port);

    return check(!session.acquire(AudioSessionFormat{44100, 2, 24}), "unsupported format is refused") &&
           check(!session.held(), "a refused acquire holds nothing") &&
           check(port.events.empty(), "a refused acquire issues no codec calls") &&
           check(!session.last_error().empty(), "a refused acquire explains itself");
}

bool test_failed_microphone_release_aborts_without_retiming() {
    FakePort port;
    port.fail_disable_input = true;
    MediaAudioSession session(port);

    return check(!session.acquire(kHiFi), "acquire fails when the microphone cannot be released") &&
           check(!session.held(), "a failed acquire holds nothing") &&
           check(port.events == std::vector<Event>{Event::DisableInput},
                 "the output is never retimed after a failed handover") &&
           check(!session.last_error().empty(), "the failure is explained");
}

bool test_failed_retiming_rolls_back_to_the_ai_channel() {
    FakePort port;
    port.fail_reconfigure_to = kHiFi;
    MediaAudioSession session(port);

    if (!check(!session.acquire(kHiFi), "acquire fails when the output cannot be retimed") ||
        !check(!session.held(), "a failed acquire holds nothing")) {
        return false;
    }

    // Rollback must retime the output explicitly: a failed reconfigure may have
    // left the I2S channel half-configured, so restoring the AI clock is not
    // optional even though the media format never took effect.
    return check(port.events == std::vector<Event>{Event::DisableInput, Event::Reconfigure,
                                                   Event::Reconfigure, Event::EnableInput},
                 "rollback retimes the output back and hands the microphone back") &&
           check(port.reconfigured.back() == kAi, "rollback restores the AI format") &&
           check(port.input_enabled(), "the AI path is usable again after rollback") &&
           check(!session.degraded(), "a successful rollback is not degraded");
}

bool test_failed_rollback_is_reported_as_degraded() {
    FakePort port;
    port.fail_reconfigure_to = kHiFi;
    port.fail_enable_input = true;
    MediaAudioSession session(port);

    return check(!session.acquire(kHiFi), "acquire still fails") &&
           check(session.degraded(), "an unrecoverable rollback is reported as degraded") &&
           check(!session.last_error().empty(), "the degraded state is explained");
}

bool test_repeated_acquire_and_release_are_idempotent() {
    FakePort port;
    MediaAudioSession session(port);
    if (!check(session.acquire(kHiFi), "first acquire succeeds")) return false;
    port.events.clear();

    if (!check(session.acquire(kHiFi), "re-acquiring the same format succeeds") ||
        !check(port.events.empty(), "re-acquiring the same format issues no codec calls")) {
        return false;
    }

    if (!check(session.release(), "release succeeds")) return false;
    port.events.clear();
    return check(session.release(), "releasing twice succeeds") &&
           check(port.events.empty(), "releasing twice issues no codec calls");
}

bool test_switching_media_format_keeps_the_original_ai_format() {
    FakePort port;
    MediaAudioSession session(port);
    if (!check(session.acquire(kHiFi), "44.1k acquire succeeds")) return false;
    if (!check(session.acquire(AudioSessionFormat{48000, 2, 16}), "48k re-acquire succeeds")) return false;

    if (!check(port.output_format().sample_rate == 48000, "output follows the new media format") ||
        !check(session.release(), "release succeeds")) {
        return false;
    }

    // The saved format must still be the AI channel, not the previous track.
    return check(port.output_format().sample_rate == 24000 && port.output_format().channels == 1,
                 "release restores the original AI format, not the previous media format");
}

bool test_destruction_hands_the_channel_back() {
    FakePort port;
    {
        MediaAudioSession session(port);
        if (!check(session.acquire(kHiFi), "acquire succeeds")) return false;
    }

    // Mooncake's uninstallAllApps() destroys apps without calling onClose(), so
    // the AI channel must also be restored by destruction alone -- otherwise
    // switching to the AI mode mid-playback leaves it at the music clock.
    return check(port.output_format() == kAi, "destruction restores the AI format") &&
           check(port.input_enabled(), "destruction hands the microphone back");
}

bool test_failed_release_is_reported_as_degraded() {
    FakePort port;
    MediaAudioSession session(port);
    if (!check(session.acquire(kHiFi), "acquire succeeds")) return false;
    port.fail_reconfigure_to = kAi;

    return check(!session.release(), "release fails when the AI format cannot be restored") &&
           check(session.degraded(), "a failed release is reported as degraded") &&
           check(!session.last_error().empty(), "the failure is explained");
}

}  // namespace

int main() {
    const bool ok = test_format_validation_rejects_unusable_clocks() &&
                    test_acquire_releases_microphone_before_retiming_output() &&
                    test_release_restores_the_ai_duplex_channel() &&
                    test_invalid_format_never_touches_the_codec() &&
                    test_failed_microphone_release_aborts_without_retiming() &&
                    test_failed_retiming_rolls_back_to_the_ai_channel() &&
                    test_failed_rollback_is_reported_as_degraded() &&
                    test_repeated_acquire_and_release_are_idempotent() &&
                    test_switching_media_format_keeps_the_original_ai_format() &&
                    test_destruction_hands_the_channel_back() &&
                    test_failed_release_is_reported_as_degraded();
    return ok ? 0 : 1;
}
