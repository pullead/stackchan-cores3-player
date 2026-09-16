#pragma once

#include "media/audio/media_audio_session.h"

class CoreS3AudioCodec;

namespace media {

// Binds MediaAudioSession to the real CoreS3 codec.  Device-only: it is not
// part of any host test, which exercises MediaAudioSession through a fake port.
class BoardAudioSessionPort final : public AudioSessionPort {
public:
    explicit BoardAudioSessionPort(CoreS3AudioCodec& codec) noexcept : codec_(codec) {}

    AudioSessionFormat output_format() const noexcept override;
    bool input_enabled() const noexcept override;
    bool set_input_enabled(bool enabled) noexcept override;
    bool reconfigure_output(const AudioSessionFormat& format) noexcept override;

private:
    CoreS3AudioCodec& codec_;
};

}  // namespace media
