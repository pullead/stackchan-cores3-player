#include "media/audio/core_s3_speaker_sink.h"

#include "media/audio/volume_policy.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>

#define TAG "MediaSink"
#endif

namespace media {

CoreS3SpeakerSink::CoreS3SpeakerSink(AudioCodecPort& codec, MediaAudioSession* session)
    : codec_(codec), session_(session) {}

bool CoreS3SpeakerSink::open(const PcmFormat& format) {
    // The sink accepts whatever the codec can actually be clocked at, so the
    // AudioSessionFormat whitelist is the single source of truth here.
    const AudioSessionFormat session_format{format.sample_rate, format.channels,
                                            format.bits_per_sample};
    if (!format.valid() || !session_format.valid()) {
        return false;
    }

    // Retime the shared channel before enabling the speaker: enabling output at
    // the AI path's rate and retiming afterwards would emit a burst at the
    // wrong pitch.
    if (session_ != nullptr && !session_->acquire(session_format)) {
        return false;
    }

    codec_.set_volume(kMutedVolumePercent);
    open_ = codec_.enable_output(true);
    if (!open_) {
        if (session_ != nullptr) session_->release();
        return false;
    }

    // Enabling the output re-applies the volume persisted in NVS, which would
    // undo the mute above.  Re-assert it after the codec is open so the muted
    // test policy cannot be defeated by a stored volume.
    codec_.set_volume(kMutedVolumePercent);

    channels_ = format.channels;
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Sink open at %u Hz, %u ch, %u bit",
             static_cast<unsigned>(format.sample_rate), static_cast<unsigned>(format.channels),
             static_cast<unsigned>(format.bits_per_sample));
#endif
    return true;
}

size_t CoreS3SpeakerSink::write(const int16_t* pcm, size_t frames) {
    if (!open_ || pcm == nullptr || channels_ == 0) {
        return 0;
    }
    const size_t written_samples = codec_.write_samples(pcm, frames * channels_);
    return written_samples / channels_;
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
    channels_ = 0;
    if (session_ != nullptr) {
        session_->release();
    }
}

}  // namespace media
