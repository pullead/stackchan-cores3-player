#include "media/audio/board_audio_codec_port.h"

#include "hal/board/cores3_audio_codec.h"

#include <audio/audio_codec.h>

#include <vector>

namespace media {

BoardAudioCodecPort::BoardAudioCodecPort(AudioCodec& codec) : codec_(codec) {}

void BoardAudioCodecPort::set_volume(uint8_t volume) {
    codec_.SetOutputVolume(volume);
}

bool BoardAudioCodecPort::enable_output(bool enabled) {
    codec_.EnableOutput(enabled);
    return codec_.output_enabled() == enabled;
}

size_t BoardAudioCodecPort::write_mono(const int16_t* samples, size_t frames) {
    if (samples == nullptr || frames == 0) {
        return 0;
    }

    std::vector<int16_t> chunk(samples, samples + frames);
    codec_.OutputData(chunk);
    return frames;
}

}  // namespace media
