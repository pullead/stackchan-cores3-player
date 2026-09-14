#include "media/audio/core_s3_speaker_sink.h"

#include "media/audio/volume_policy.h"

namespace media {

CoreS3SpeakerSink::CoreS3SpeakerSink(AudioCodecPort& codec) : codec_(codec) {}

bool CoreS3SpeakerSink::open(const PcmFormat& format) {
    if (!format.valid() || format.sample_rate != 24000 || format.channels != 1) {
        return false;
    }

    codec_.set_volume(kMutedVolumePercent);
    open_ = codec_.enable_output(true);
    return open_;
}

size_t CoreS3SpeakerSink::write(const int16_t* pcm, size_t frames) {
    if (!open_ || pcm == nullptr) {
        return 0;
    }
    return codec_.write_mono(pcm, frames);
}

void CoreS3SpeakerSink::pause() {
    codec_.set_volume(kMutedVolumePercent);
}

void CoreS3SpeakerSink::flush() {}

void CoreS3SpeakerSink::close() {
    codec_.set_volume(kMutedVolumePercent);
    if (open_) {
        codec_.enable_output(false);
        open_ = false;
    }
}

}  // namespace media
