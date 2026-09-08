#include "media/local/local_playback_controller.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum class Event {
    Open,
    Write,
    Pause,
    Flush,
    Close,
};

class FakeSink final : public media::AudioSink {
public:
    bool open(const media::PcmFormat& format) override {
        events.push_back(Event::Open);
        opened_format = format;
        return open_result;
    }

    size_t write(const int16_t*, size_t frames) override {
        events.push_back(Event::Write);
        requested_frames.push_back(frames);
        return write_result_set ? write_result : frames;
    }

    void pause() override { events.push_back(Event::Pause); }
    void flush() override { events.push_back(Event::Flush); }
    void close() override { events.push_back(Event::Close); }

    bool open_result = true;
    bool write_result_set = false;
    size_t write_result = 0;
    media::PcmFormat opened_format{};
    std::vector<size_t> requested_frames;
    std::vector<Event> events;
};

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void append_id(std::vector<uint8_t>& bytes, const char (&id)[5]) {
    bytes.insert(bytes.end(), id, id + 4);
}

std::vector<uint8_t> compatible_wav(size_t frames) {
    std::vector<uint8_t> bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0);
    append_id(bytes, "WAVE");
    append_id(bytes, "fmt ");
    append_u32(bytes, 16);
    append_u16(bytes, 1);
    append_u16(bytes, 1);
    append_u32(bytes, 24000);
    append_u32(bytes, 48000);
    append_u16(bytes, 2);
    append_u16(bytes, 16);
    append_id(bytes, "data");
    append_u32(bytes, static_cast<uint32_t>(frames * 2));
    for (size_t index = 0; index < frames; ++index) {
        append_u16(bytes, static_cast<uint16_t>(index));
    }
    const uint32_t riff_size = static_cast<uint32_t>(bytes.size() - 8);
    bytes[4] = static_cast<uint8_t>(riff_size & 0xff);
    bytes[5] = static_cast<uint8_t>((riff_size >> 8) & 0xff);
    bytes[6] = static_cast<uint8_t>((riff_size >> 16) & 0xff);
    bytes[7] = static_cast<uint8_t>((riff_size >> 24) & 0xff);
    return bytes;
}

bool test_start_and_pump_follow_bounded_playback_route() {
    FakeSink sink;
    media::LocalPlaybackController controller(sink);
    controller.select("demo.wav", compatible_wav(1500));

    if (!check(controller.start(), "compatible WAV starts") ||
        !check(controller.snapshot().state == media::PlaybackState::Playing,
               "start reaches Playing through preparing and buffering") ||
        !check(sink.events == std::vector<Event>{Event::Open}, "start only opens sink") ||
        !check(sink.opened_format.sample_rate == 24000 && sink.opened_format.channels == 1 &&
                   sink.opened_format.bits_per_sample == 16,
               "only 24k mono S16 format is opened")) {
        return false;
    }

    controller.pump();
    if (!check(controller.snapshot().state == media::PlaybackState::Playing,
               "non-final pump remains Playing") ||
        !check(controller.snapshot().total_frames == 1500, "snapshot reports total frames") ||
        !check(controller.snapshot().played_frames == 1024, "pump writes bounded chunk") ||
        !check(sink.requested_frames == std::vector<size_t>{1024}, "first write is at most 1024")) {
        return false;
    }

    controller.pump();
    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "EOF returns to Idle") &&
           check(snapshot.played_frames == 1500, "EOF retains played frame count") &&
           check(snapshot.title == "demo.wav", "snapshot retains selection title") &&
           check(snapshot.muted, "controller snapshot is always muted") &&
           check(sink.requested_frames == std::vector<size_t>{1024, 476},
                 "final write is remaining frames") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Write,
                                                    Event::Flush, Event::Close},
                 "EOF flushes then closes");
}

bool test_malformed_selection_never_opens_sink() {
    auto malformed = compatible_wav(1);
    malformed[0] = 'X';
    FakeSink sink;
    media::LocalPlaybackController controller(sink);
    controller.select("broken.wav", malformed);

    const auto started = controller.start();
    const auto snapshot = controller.snapshot();
    return check(!started, "malformed WAV does not start") &&
           check(snapshot.state == media::PlaybackState::Idle, "parse error returns Idle") &&
           check(!snapshot.error.empty(), "parse error is exposed") &&
           check(sink.events.empty(), "parse failure never opens sink");
}

bool test_partial_write_flushes_and_closes() {
    FakeSink sink;
    sink.write_result_set = true;
    sink.write_result = 10;
    media::LocalPlaybackController controller(sink);
    controller.select("partial.wav", compatible_wav(20));
    if (!check(controller.start(), "partial-write fixture starts")) {
        return false;
    }
    controller.pump();

    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "partial write returns Idle") &&
           check(snapshot.played_frames == 10, "partial write counts accepted frames") &&
           check(!snapshot.error.empty(), "partial write is exposed as error") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Flush,
                                                    Event::Close},
                 "partial write flushes before close");
}

bool test_zero_write_flushes_and_closes() {
    FakeSink sink;
    sink.write_result_set = true;
    sink.write_result = 0;
    media::LocalPlaybackController controller(sink);
    controller.select("zero.wav", compatible_wav(20));
    if (!check(controller.start(), "zero-write fixture starts")) {
        return false;
    }
    controller.pump();

    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "zero write returns Idle") &&
           check(snapshot.played_frames == 0, "zero write makes no progress") &&
           check(!snapshot.error.empty(), "zero write is exposed as error") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Flush,
                                                    Event::Close},
                 "zero write flushes before close");
}

bool test_stop_for_ai_flushes_before_close() {
    FakeSink sink;
    media::LocalPlaybackController controller(sink);
    controller.select("ai.wav", compatible_wav(20));
    if (!check(controller.start(), "AI handoff fixture starts")) {
        return false;
    }
    controller.stop_for_ai();

    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "AI handoff returns Idle") &&
           check(snapshot.muted, "AI handoff remains muted") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Flush, Event::Close},
                 "AI handoff flushes before close");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_start_and_pump_follow_bounded_playback_route();
    failures += !test_malformed_selection_never_opens_sink();
    failures += !test_partial_write_flushes_and_closes();
    failures += !test_zero_write_flushes_and_closes();
    failures += !test_stop_for_ai_flushes_before_close();
    return failures == 0 ? 0 : 1;
}
