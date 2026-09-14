#include "media/decoder/hifi_decoder_adapter.h"

#include <cassert>
#include <cstdint>
#include <cstring>

using namespace media;

class FixtureStream final : public AudioStream {
public:
    AudioStreamStatus read(uint8_t*, std::size_t, std::size_t& n) noexcept override { n = 0; return AudioStreamStatus::Eof; }
    AudioStreamStatus seek(uint64_t) noexcept override { return AudioStreamStatus::Ok; }
    uint64_t tell() const noexcept override { return 0; }
    uint64_t size() const noexcept override { return 4; }
    bool is_open() const noexcept override { return true; }
    AudioStreamStatus close() noexcept override { return AudioStreamStatus::Ok; }
};

class FixtureBackend final : public HifiDecoderBackend {
public:
    AudioDecodeStatus open(AudioStream&) noexcept override { opened = true; return malformed ? AudioDecodeStatus::Malformed : AudioDecodeStatus::Ok; }
    AudioDecodeStatus decode(PcmBlock& block) noexcept override {
        if (!opened) return AudioDecodeStatus::NotOpen;
        if (done) return AudioDecodeStatus::Eof;
        block.samples[0] = 123; block.frames = 1; done = true; return AudioDecodeStatus::Ok;
    }
    const PcmFormat& format() const noexcept override { return pcm; }
    const AudioMetadata& metadata() const noexcept override { return info; }
    bool eof() const noexcept override { return done; }
    AudioDecodeStatus last_error() const noexcept override { return malformed ? AudioDecodeStatus::Malformed : AudioDecodeStatus::Ok; }
    bool opened = false, done = false, malformed = false;
    PcmFormat pcm{44100, 2, 16};
    AudioMetadata info{"title", "artist", "album"};
};

int main() {
    FixtureStream stream; FixtureBackend backend; HifiDecoderAdapter decoder(backend);
    int16_t samples[2]{}; PcmBlock block{samples, 1, 0};
    assert(decoder.open(stream) == AudioDecodeStatus::Ok);
    assert(decoder.metadata().title == "title");
    assert(decoder.format().sample_rate == 44100);
    assert(decoder.decode(block) == AudioDecodeStatus::Ok && block.frames == 1);
    block.frames = 0; assert(decoder.decode(block) == AudioDecodeStatus::Eof && decoder.eof());
    PcmBlock bad{nullptr, 1, 0}; assert(decoder.decode(bad) == AudioDecodeStatus::InvalidArgument);
    FixtureBackend broken; broken.malformed = true; HifiDecoderAdapter malformed(broken);
    assert(malformed.open(stream) == AudioDecodeStatus::Malformed);
    return 0;
}
