#include "media/audio/media_audio_session.h"

#include <utility>

namespace media {
namespace {

// Rates the AW88298 path is driven at in practice: the AI channel's 24 kHz,
// the common music rates, and their usual relatives.  Anything else would need
// resampling before it reaches this layer.
constexpr uint32_t kSupportedRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

}  // namespace

bool AudioSessionFormat::valid() const noexcept {
    if (channels < 1 || channels > 2 || bits_per_sample != 16) {
        return false;
    }
    for (const uint32_t rate : kSupportedRates) {
        if (sample_rate == rate) return true;
    }
    return false;
}

bool MediaAudioSession::fail(std::string error) noexcept {
    last_error_ = std::move(error);
    return false;
}

bool MediaAudioSession::acquire(const AudioSessionFormat& desired) noexcept {
    if (!desired.valid()) {
        return fail("Unsupported media audio format");
    }
    if (held_ && port_.output_format() == desired) {
        return true;
    }

    // Remember the AI channel only on the first handover: re-acquiring with a
    // different media format must not record a media format as the AI one.
    if (!held_) {
        restore_format_ = port_.output_format();
        restore_input_ = port_.input_enabled();
        if (restore_input_ && !port_.set_input_enabled(false)) {
            return fail("Could not release the microphone for media playback");
        }
    }

    if (port_.reconfigure_output(desired)) {
        held_ = true;
        last_error_.clear();
        return true;
    }

    if (held_) {
        // A format switch while already holding the channel leaves the previous
        // media configuration running; the AI channel is not at risk.
        return fail("Could not retime the output to the new media format");
    }

    // Roll back the handover so the AI path is left exactly as it was found.
    bool rolled_back = port_.reconfigure_output(restore_format_);
    if (restore_input_ && !port_.set_input_enabled(true)) {
        rolled_back = false;
    }
    degraded_ = !rolled_back;
    return fail(degraded_ ? "Could not retime the output, and restoring the AI channel failed"
                          : "Could not retime the output for media playback");
}

bool MediaAudioSession::release() noexcept {
    if (!held_) {
        return true;
    }

    bool restored = port_.reconfigure_output(restore_format_);
    if (restore_input_ && !port_.set_input_enabled(true)) {
        restored = false;
    }
    held_ = false;

    if (!restored) {
        degraded_ = true;
        return fail("Could not restore the AI audio channel");
    }

    last_error_.clear();
    return true;
}

}  // namespace media
