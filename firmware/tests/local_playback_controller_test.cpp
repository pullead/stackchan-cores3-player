#include "media/local/local_playback_controller.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>
#include <memory>

static_assert(!std::is_copy_constructible_v<media::LocalPlaybackController>);
static_assert(!std::is_copy_assignable_v<media::LocalPlaybackController>);
static_assert(!std::is_move_constructible_v<media::LocalPlaybackController>);
static_assert(!std::is_move_assignable_v<media::LocalPlaybackController>);

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

    size_t write(const int16_t* pcm, size_t frames) override {
        events.push_back(Event::Write);
        requested_frames.push_back(frames);
        written_pcm.emplace_back(pcm, pcm + frames);
        return write_results.empty() ? frames : next_write_result();
    }

    void pause() override { events.push_back(Event::Pause); }
    void flush() override { events.push_back(Event::Flush); }
    void close() override { events.push_back(Event::Close); }

    bool open_result = true;
    std::vector<size_t> write_results;
    media::PcmFormat opened_format{};
    std::vector<size_t> requested_frames;
    std::vector<std::vector<int16_t>> written_pcm;
    std::vector<Event> events;

private:
    size_t next_write_result() {
        const size_t index = write_result_index < write_results.size() ? write_result_index
                                                                        : write_results.size() - 1;
        ++write_result_index;
        return write_results[index];
    }

    size_t write_result_index = 0;
};

class FakeStream final : public media::AudioStream {
public:
    media::AudioStreamStatus read(uint8_t*, size_t, size_t& count) noexcept override { count = 0; return media::AudioStreamStatus::Eof; }
    media::AudioStreamStatus seek(uint64_t) noexcept override { return media::AudioStreamStatus::Ok; }
    uint64_t tell() const noexcept override { return 0; }
    uint64_t size() const noexcept override { return 0; }
    bool is_open() const noexcept override { return open_; }
    media::AudioStreamStatus close() noexcept override { open_ = false; closed = true; return media::AudioStreamStatus::Ok; }
    bool closed = false;
private:
    bool open_ = true;
};

class FakeDecoder final : public media::AudioDecoder {
public:
    media::AudioDecodeStatus open(media::AudioStream&) noexcept override { opened = true; return open_result; }
    media::AudioDecodeStatus decode(media::PcmBlock& block) noexcept override {
        if (decode_error) return media::AudioDecodeStatus::IoError;
        if (invalid_block) { block.frames = block.capacity_frames + 1; return media::AudioDecodeStatus::Ok; }
        if (done) return media::AudioDecodeStatus::Eof;
        block.samples[0] = 11; block.samples[1] = 22; block.frames = 2; done = true; return media::AudioDecodeStatus::Ok;
    }
    const media::PcmFormat& format() const noexcept override { return pcm; }
    const media::AudioMetadata& metadata() const noexcept override { return metadata_; }
    bool eof() const noexcept override { return done; }
    media::AudioDecodeStatus last_error() const noexcept override { return media::AudioDecodeStatus::IoError; }
    media::PcmFormat pcm{48000, 1, 16};
    media::AudioMetadata metadata_{};
    media::AudioDecodeStatus open_result = media::AudioDecodeStatus::Ok;
    bool decode_error = false;
    bool invalid_block = false;
    bool opened = false;
    bool done = false;
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

bool test_short_write_retries_pending_samples_without_gaps() {
    FakeSink sink;
    sink.write_results = {10, 10};
    media::LocalPlaybackController controller(sink);
    controller.select("partial.wav", compatible_wav(20));
    if (!check(controller.start(), "short-write fixture starts")) {
        return false;
    }
    controller.pump();

    if (!check(controller.snapshot().state == media::PlaybackState::Playing,
               "short write keeps controller Playing") ||
        !check(controller.snapshot().played_frames == 10, "short write counts accepted frames") ||
        !check(sink.requested_frames == std::vector<size_t>{20}, "first write receives full chunk")) {
        return false;
    }

    controller.pump();
    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "completed short write returns Idle at EOF") &&
           check(snapshot.played_frames == 20, "retried samples complete frame count") &&
           check(snapshot.error.empty(), "short write is not an error") &&
           check(sink.requested_frames == std::vector<size_t>{20, 10},
                 "second write contains only pending frames") &&
           check(sink.written_pcm[0][10] == 10 && sink.written_pcm[1][0] == 10 &&
                     sink.written_pcm[1][9] == 19,
                 "pending sample sequence resumes without a gap") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Write,
                                                    Event::Flush, Event::Close},
                 "EOF still flushes before close");
}

bool test_zero_write_flushes_and_closes() {
    FakeSink sink;
    sink.write_results = {0};
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

bool test_overreported_write_flushes_and_closes() {
    FakeSink sink;
    sink.write_results = {21};
    media::LocalPlaybackController controller(sink);
    controller.select("overreported.wav", compatible_wav(20));
    if (!check(controller.start(), "overreported-write fixture starts")) {
        return false;
    }
    controller.pump();

    const auto snapshot = controller.snapshot();
    return check(snapshot.state == media::PlaybackState::Idle, "overreported write returns Idle") &&
           check(snapshot.played_frames == 0, "overreported write makes no trusted progress") &&
           check(!snapshot.error.empty(), "overreported write is exposed as error") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Flush,
                                                    Event::Close},
                 "overreported write flushes before close");
}

bool test_open_failure_flushes_and_closes() {
    FakeSink sink;
    sink.open_result = false;
    media::LocalPlaybackController controller(sink);
    controller.select("open-failure.wav", compatible_wav(20));

    const bool started = controller.start();
    const auto snapshot = controller.snapshot();
    return check(!started, "open failure does not start") &&
           check(snapshot.state == media::PlaybackState::Idle, "open failure returns Idle") &&
           check(!snapshot.error.empty(), "open failure is exposed as error") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Flush, Event::Close},
                 "failed open still flushes then closes") &&
           check(sink.requested_frames.empty(), "failed open never writes PCM");
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

bool test_decoder_stream_route_writes_bounded_pcm_and_closes() {
    FakeSink sink;
    auto* stream = new FakeStream();
    auto* decoder = new FakeDecoder();
    media::LocalPlaybackController controller(sink);
    controller.select("demo.mp3", std::unique_ptr<media::AudioStream>(stream),
                      std::unique_ptr<media::AudioDecoder>(decoder));
    if (!check(controller.start(), "decoder route starts") || !check(decoder->opened, "decoder opens stream")) return false;
    controller.pump();
    const auto playing = controller.snapshot();
    if (!check(playing.state == media::PlaybackState::Playing, "decoded PCM is playing") ||
        !check(playing.played_frames == 2, "decoded frames reach sink") ||
        !check(sink.written_pcm[0] == std::vector<int16_t>{11, 22}, "decoded PCM is preserved")) return false;
    controller.pump();
    return check(controller.snapshot().state == media::PlaybackState::Idle, "decoder EOF returns idle") &&
           check(stream->closed, "EOF closes stream") && check(controller.snapshot().muted, "decoder route remains muted");
}

bool test_decoder_error_closes_everything_without_writing() {
    FakeSink sink;
    auto* stream = new FakeStream();
    auto* decoder = new FakeDecoder();
    decoder->decode_error = true;
    media::LocalPlaybackController controller(sink);
    controller.select("broken.mp3", std::unique_ptr<media::AudioStream>(stream),
                      std::unique_ptr<media::AudioDecoder>(decoder));
    if (!check(controller.start(), "error decoder opens before decode")) return false;
    controller.pump();
    return check(controller.snapshot().state == media::PlaybackState::Idle, "decoder error returns idle") &&
           check(!controller.snapshot().error.empty(), "decoder error is exposed") &&
           check(stream->closed, "decoder error closes stream") &&
           check(sink.requested_frames.empty(), "decoder error writes no PCM") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Flush, Event::Close}, "decoder error closes sink");
}

bool test_decoder_stop_closes_stream_and_sink() {
    FakeSink sink;
    auto* stream = new FakeStream();
    auto* decoder = new FakeDecoder();
    media::LocalPlaybackController controller(sink);
    controller.select("stop.mp3", std::unique_ptr<media::AudioStream>(stream),
                      std::unique_ptr<media::AudioDecoder>(decoder));
    if (!check(controller.start(), "stop fixture starts")) return false;
    controller.stop();
    return check(stream->closed, "stop closes stream") &&
           check(sink.events == std::vector<Event>{Event::Open, Event::Flush, Event::Close}, "stop closes sink");
}

bool test_invalid_decoder_block_is_rejected_without_writing() {
    FakeSink sink;
    auto* stream = new FakeStream();
    auto* decoder = new FakeDecoder();
    decoder->invalid_block = true;
    media::LocalPlaybackController controller(sink);
    controller.select("invalid.mp3", std::unique_ptr<media::AudioStream>(stream),
                      std::unique_ptr<media::AudioDecoder>(decoder));
    if (!check(controller.start(), "invalid block fixture starts")) return false;
    controller.pump();
    return check(!sink.requested_frames.size(), "invalid block never reaches sink") &&
           check(stream->closed, "invalid block closes stream") &&
           check(!controller.snapshot().error.empty(), "invalid block exposes error");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_start_and_pump_follow_bounded_playback_route();
    failures += !test_malformed_selection_never_opens_sink();
    failures += !test_short_write_retries_pending_samples_without_gaps();
    failures += !test_zero_write_flushes_and_closes();
    failures += !test_overreported_write_flushes_and_closes();
    failures += !test_open_failure_flushes_and_closes();
    failures += !test_stop_for_ai_flushes_before_close();
    failures += !test_decoder_stream_route_writes_bounded_pcm_and_closes();
    failures += !test_decoder_error_closes_everything_without_writing();
    failures += !test_decoder_stop_closes_stream_and_sink();
    failures += !test_invalid_decoder_block_is_rejected_without_writing();
    return failures == 0 ? 0 : 1;
}
