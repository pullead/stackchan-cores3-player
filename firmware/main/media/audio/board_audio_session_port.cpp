#include "media/audio/board_audio_session_port.h"

#include "hal/board/cores3_audio_codec.h"

namespace media {

AudioSessionFormat BoardAudioSessionPort::output_format() const noexcept {
    return AudioSessionFormat{static_cast<uint32_t>(codec_.output_sample_rate()),
                              static_cast<uint8_t>(codec_.output_channels()), 16};
}

bool BoardAudioSessionPort::input_enabled() const noexcept { return codec_.input_enabled(); }

bool BoardAudioSessionPort::set_input_enabled(bool enabled) noexcept {
    codec_.EnableInput(enabled);
    return codec_.input_enabled() == enabled;
}

bool BoardAudioSessionPort::reconfigure_output(const AudioSessionFormat& format) noexcept {
    return codec_.ReconfigureOutput(static_cast<int>(format.sample_rate),
                                    static_cast<int>(format.channels));
}

}  // namespace media
