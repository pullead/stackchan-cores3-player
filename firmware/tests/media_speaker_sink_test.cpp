#include "media/audio/core_s3_speaker_sink.h"
#include "media/audio/media_audio_session.h"
#include "media/audio/volume_policy.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

enum class Call : uint8_t {
    Mute = 1,
    Enable = 2,
    Write = 3,
    Disable = 4,
    NonMute = 99,
};

class FakeCodecPort final : public media::AudioCodecPort {
public:
    void set_volume(uint8_t volume) override {
        volume_ = volume;
        calls_.push_back(volume == media::kMutedVolumePercent ? Call::Mute : Call::NonMute);
    }

    bool enable_output(bool enabled) override {
        calls_.push_back(enabled ? Call::Enable : Call::Disable);
        return enabled ? enable_succeeds_ : true;
    }

    size_t write_samples(const int16_t*, size_t samples) override {
        calls_.push_back(Call::Write);
        written_samples.push_back(samples);
        return samples;
    }

    void set_enable_succeeds(bool succeeds) { enable_succeeds_ = succeeds; }
    uint8_t volume() const { return volume_; }
    const std::vector<Call>& calls() const { return calls_; }

    std::vector<size_t> written_samples;

private:
    uint8_t volume_ = 255;
    bool enable_succeeds_ = true;
    std::vector<Call> calls_;
};

// The AI voice path owns a 24 kHz mono duplex channel until media takes over.
class FakeSessionPort final : public media::AudioSessionPort {
public:
    media::AudioSessionFormat output_format() const noexcept override { return format_; }
    bool input_enabled() const noexcept override { return input_enabled_; }
    bool set_input_enabled(bool enabled) noexcept override {
        input_enabled_ = enabled;
        return true;
    }
    bool reconfigure_output(const media::AudioSessionFormat& format) noexcept override {
        if (refuse_retiming) return false;
        format_ = format;
        return true;
    }

    bool refuse_retiming = false;

private:
    media::AudioSessionFormat format_{24000, 1, 16};
    bool input_enabled_ = true;
};

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

bool check_calls(const std::vector<Call>& actual, std::initializer_list<Call> expected,
                 const char* expression) {
    if (actual.size() == expected.size()) {
        size_t index = 0;
        for (const Call call : expected) {
            if (actual[index++] != call) {
                std::fprintf(stderr, "check failed: %s (call mismatch at %zu)\n", expression,
                             index - 1);
                return false;
            }
        }
        return true;
    }
    std::fprintf(stderr, "check failed: %s (expected %zu calls, got %zu)\n", expression,
                 expected.size(), actual.size());
    return false;
}

media::PcmFormat supported_format() {
    return {44100, 2, 16};
}

bool test_open_mutes_before_enabling() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const bool opened = sink.open(supported_format());

    return check(opened, "supported open succeeds") &&
           check(codec.volume() == media::kMutedVolumePercent, "open leaves volume muted") &&
           check_calls(codec.calls(), {Call::Mute, Call::Enable}, "open calls mute then enable");
}

bool test_common_music_formats_are_accepted() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink stereo_44k(codec);
    media::CoreS3SpeakerSink stereo_48k(codec);
    media::CoreS3SpeakerSink mono_24k(codec);
    media::CoreS3SpeakerSink mono_22k(codec);

    return check(stereo_44k.open({44100, 2, 16}), "44.1k stereo is accepted") &&
           check(stereo_48k.open({48000, 2, 16}), "48k stereo is accepted") &&
           check(mono_24k.open({24000, 1, 16}), "the AI 24k mono format still works") &&
           check(mono_22k.open({22050, 1, 16}), "22.05k mono is accepted");
}

bool test_stereo_write_forwards_interleaved_samples() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const int16_t stereo[] = {1, 2, 3, 4, 5, 6};
    sink.open({44100, 2, 16});
    const size_t written = sink.write(stereo, 3);

    // Three stereo frames are six interleaved samples; reporting frames back
    // keeps the controller's bookkeeping in frames, not samples.
    return check(written == 3, "write reports frames, not samples") &&
           check(codec.written_samples == std::vector<size_t>{6},
                 "stereo frames reach the codec as interleaved samples");
}

bool test_mono_write_forwards_frames() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const int16_t silence[] = {0, 0, 0};
    sink.open({24000, 1, 16});
    const size_t written = sink.write(silence, 3);

    return check(written == 3, "write forwards all silence frames") &&
           check(codec.written_samples == std::vector<size_t>{3},
                 "mono frames map one-to-one to samples");
}

bool test_pause_and_close_remain_hard_muted() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    sink.open(supported_format());
    sink.pause();
    sink.close();

    const auto& calls = codec.calls();
    return check(codec.volume() == media::kMutedVolumePercent, "pause and close leave volume muted") &&
           check(calls.size() >= 4, "pause and close produce calls") &&
           check(calls[calls.size() - 2] == Call::Mute && calls.back() == Call::Disable,
                 "close mutes before disable");
}

bool test_wrong_format_has_no_codec_calls() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    media::CoreS3SpeakerSink wide(codec);
    media::CoreS3SpeakerSink off_grid(codec);

    return check(!sink.open({44100, 3, 16}), "three channels is rejected") &&
           check(!wide.open({44100, 2, 24}), "24-bit is rejected") &&
           check(!off_grid.open({44101, 2, 16}), "an unachievable clock is rejected") &&
           check(codec.calls().empty(), "wrong format does not touch codec");
}

bool test_closed_or_null_write_is_rejected() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const int16_t silence[] = {0};

    const bool closed_rejected = sink.write(silence, 1) == 0;
    sink.open(supported_format());
    const bool null_rejected = sink.write(nullptr, 1) == 0;
    return check(closed_rejected, "closed write returns zero") &&
           check(null_rejected, "null write returns zero") &&
           check_calls(codec.calls(), {Call::Mute, Call::Enable}, "rejected writes do not reach codec");
}

bool test_failed_enable_stays_closed() {
    FakeCodecPort codec;
    codec.set_enable_succeeds(false);
    media::CoreS3SpeakerSink sink(codec);

    const bool opened = sink.open(supported_format());
    sink.close();
    return check(!opened, "failed enable makes open fail") &&
           check_calls(codec.calls(), {Call::Mute, Call::Enable, Call::Mute},
                       "failed open mutes before enable and close avoids disable");
}

bool test_open_takes_the_audio_channel_from_the_ai_path() {
    FakeCodecPort codec;
    FakeSessionPort port;
    media::MediaAudioSession session(port);
    media::CoreS3SpeakerSink sink(codec, &session);

    if (!check(sink.open({44100, 2, 16}), "open succeeds with a session")) return false;

    const bool acquired =
        check(session.held(), "open acquires the audio session") &&
        check(port.output_format().sample_rate == 44100 && port.output_format().channels == 2,
              "the codec is retimed to the track format") &&
        check(!port.input_enabled(), "the microphone is released while music plays");

    sink.close();
    return acquired && check(!session.held(), "close releases the audio session") &&
           check(port.output_format().sample_rate == 24000,
                 "close hands the AI channel back at 24k") &&
           check(port.input_enabled(), "close hands the microphone back");
}

bool test_failed_retiming_keeps_the_output_disabled() {
    FakeCodecPort codec;
    FakeSessionPort port;
    port.refuse_retiming = true;
    media::MediaAudioSession session(port);
    media::CoreS3SpeakerSink sink(codec, &session);

    return check(!sink.open({44100, 2, 16}), "open fails when the channel cannot be retimed") &&
           check(!session.held(), "a failed open holds no session") &&
           check(codec.calls().empty(), "a failed retiming never enables the speaker");
}

bool test_destroying_an_open_sink_releases_the_channel() {
    FakeCodecPort codec;
    FakeSessionPort port;
    media::MediaAudioSession session(port);
    {
        media::CoreS3SpeakerSink sink(codec, &session);
        if (!check(sink.open({44100, 2, 16}), "open succeeds")) return false;
    }

    // Apps can be destroyed without onClose() ever running, so an open sink has
    // to release the speaker and the audio channel on destruction too.
    return check(!session.held(), "destruction releases the audio session") &&
           check(codec.calls().back() == Call::Disable, "destruction disables the speaker") &&
           check(port.input_enabled(), "destruction hands the microphone back");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_open_mutes_before_enabling();
    failures += !test_common_music_formats_are_accepted();
    failures += !test_stereo_write_forwards_interleaved_samples();
    failures += !test_mono_write_forwards_frames();
    failures += !test_pause_and_close_remain_hard_muted();
    failures += !test_wrong_format_has_no_codec_calls();
    failures += !test_closed_or_null_write_is_rejected();
    failures += !test_failed_enable_stays_closed();
    failures += !test_open_takes_the_audio_channel_from_the_ai_path();
    failures += !test_failed_retiming_keeps_the_output_disabled();
    failures += !test_destroying_an_open_sink_releases_the_channel();
    return failures == 0 ? 0 : 1;
}
