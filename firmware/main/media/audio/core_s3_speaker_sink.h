#pragma once

#include "media/audio/audio_codec_port.h"
#include "media/audio/audio_sink.h"
#include "media/audio/media_audio_session.h"

namespace media {

// Writes decoded PCM to the CoreS3 speaker.
//
// When a MediaAudioSession is supplied, opening the sink also takes the shared
// I2S channel away from the AI voice path and retimes it to the track's format;
// closing hands it back.  Without a session the sink only validates the format,
// which is what host tests use.
class CoreS3SpeakerSink final : public AudioSink {
public:
    explicit CoreS3SpeakerSink(AudioCodecPort& codec, MediaAudioSession* session = nullptr);

    // An app can be destroyed without onClose() ever running; never leave the
    // speaker enabled or the audio channel held.
    ~CoreS3SpeakerSink() override { close(); }

    bool open(const PcmFormat& format) override;
    size_t write(const int16_t* pcm, size_t frames) override;
    void pause() override;
    void flush() override;
    void close() override;

private:
    AudioCodecPort& codec_;
    MediaAudioSession* session_ = nullptr;
    uint8_t channels_ = 0;
    bool open_ = false;
};

}  // namespace media
