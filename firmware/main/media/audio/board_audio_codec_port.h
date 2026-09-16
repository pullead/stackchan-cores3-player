#pragma once

#include "media/audio/audio_codec_port.h"

class AudioCodec;

namespace media {

class BoardAudioCodecPort final : public AudioCodecPort {
public:
    explicit BoardAudioCodecPort(AudioCodec& codec);

    void set_volume(uint8_t volume) override;
    bool enable_output(bool enabled) override;
    size_t write_samples(const int16_t* samples, size_t count) override;

private:
    AudioCodec& codec_;
};

}  // namespace media
