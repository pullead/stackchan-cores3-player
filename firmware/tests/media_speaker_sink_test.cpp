#include "media/audio/core_s3_speaker_sink.h"
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

    size_t write_mono(const int16_t*, size_t frames) override {
        calls_.push_back(Call::Write);
        return frames;
    }

    void set_enable_succeeds(bool succeeds) { enable_succeeds_ = succeeds; }
    uint8_t volume() const { return volume_; }
    const std::vector<Call>& calls() const { return calls_; }

private:
    uint8_t volume_ = 255;
    bool enable_succeeds_ = true;
    std::vector<Call> calls_;
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
    return {24000, 1, 16};
}

bool test_open_mutes_before_enabling() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const bool opened = sink.open(supported_format());

    return check(opened, "supported open succeeds") &&
           check(codec.volume() == media::kMutedVolumePercent, "open leaves volume muted") &&
           check_calls(codec.calls(), {Call::Mute, Call::Enable}, "open calls mute then enable");
}

bool test_write_silence_forwards_frames() {
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    const int16_t silence[] = {0, 0, 0};
    sink.open(supported_format());
    const size_t written = sink.write(silence, 3);

    return check(written == 3, "write forwards all silence frames") &&
           check(codec.calls().back() == Call::Write, "write reaches codec");
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

    return check(!sink.open({48000, 2, 16}), "wrong format is rejected") &&
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

}  // namespace

int main() {
    int failures = 0;
    failures += !test_open_mutes_before_enabling();
    failures += !test_write_silence_forwards_frames();
    failures += !test_pause_and_close_remain_hard_muted();
    failures += !test_wrong_format_has_no_codec_calls();
    failures += !test_closed_or_null_write_is_rejected();
    failures += !test_failed_enable_stays_closed();
    return failures == 0 ? 0 : 1;
}
