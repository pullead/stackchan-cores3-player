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

size_t BoardAudioCodecPort::write_samples(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0;
    }

    std::vector<int16_t> chunk(samples, samples + count);
    codec_.OutputData(chunk);
    return count;
}

}  // namespace media
