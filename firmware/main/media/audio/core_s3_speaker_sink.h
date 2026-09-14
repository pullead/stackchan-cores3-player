#pragma once

#include "media/audio/audio_codec_port.h"
#include "media/audio/audio_sink.h"

namespace media {

class CoreS3SpeakerSink final : public AudioSink {
public:
    explicit CoreS3SpeakerSink(AudioCodecPort& codec);

    bool open(const PcmFormat& format) override;
    size_t write(const int16_t* pcm, size_t frames) override;
    void pause() override;
    void flush() override;
    void close() override;

private:
    AudioCodecPort& codec_;
    bool open_ = false;
};

}  // namespace media
