#include "media/decoder/audio_decoder.h"

#include <cassert>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
class FakeStream final : public media::AudioStream {
public:
    explicit FakeStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    media::AudioStreamStatus read(uint8_t* dst, std::size_t capacity, std::size_t& count) noexcept override {
        if (closed_) { count = 0; return media::AudioStreamStatus::Closed; }
        if (!dst || capacity == 0) { count = 0; return media::AudioStreamStatus::InvalidArgument; }
        count = std::min(capacity, bytes_.size() - position_);
        std::copy_n(bytes_.data() + position_, count, dst);
        position_ += count;
        return count == 0 ? media::AudioStreamStatus::Eof : media::AudioStreamStatus::Ok;
    }
    media::AudioStreamStatus seek(uint64_t offset) noexcept override {
        if (closed_) return media::AudioStreamStatus::Closed;
        if (offset > bytes_.size()) return media::AudioStreamStatus::OutOfRange;
        position_ = static_cast<std::size_t>(offset); return media::AudioStreamStatus::Ok;
    }
    uint64_t tell() const noexcept override { return closed_ ? 0 : position_; }
    uint64_t size() const noexcept override { return closed_ ? 0 : bytes_.size(); }
    bool is_open() const noexcept override { return !closed_; }
    media::AudioStreamStatus close() noexcept override { closed_ = true; return media::AudioStreamStatus::Ok; }
private:
    std::vector<uint8_t> bytes_; std::size_t position_ = 0; bool closed_ = false;
};
}

int main() {
    FakeStream stream({1, 2, 3});
    uint8_t buffer[2]{}; std::size_t count = 0;
    assert(stream.is_open());
    assert(stream.read(nullptr, sizeof(buffer), count) == media::AudioStreamStatus::InvalidArgument);
    assert(stream.read(buffer, 0, count) == media::AudioStreamStatus::InvalidArgument);
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Ok);
    assert(count == 2 && buffer[0] == 1 && buffer[1] == 2);
    assert(stream.tell() == 2);
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Ok && count == 1);
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Eof && count == 0);
    assert(stream.seek(1) == media::AudioStreamStatus::Ok && stream.tell() == 1);
    assert(stream.seek(4) == media::AudioStreamStatus::OutOfRange);
    assert(stream.close() == media::AudioStreamStatus::Ok && !stream.is_open());
    assert(stream.close() == media::AudioStreamStatus::Ok && !stream.is_open());
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Closed);
    assert(stream.seek(0) == media::AudioStreamStatus::Closed);
    assert(stream.tell() == 0 && stream.size() == 0);

    media::PcmBlock block{};
    assert(!block.valid());
    int16_t samples[4]{};
    block.samples = samples;
    block.capacity_frames = 4;
    block.frames = 5;
    assert(!block.valid());
    block.frames = 4;
    assert(block.valid());
}
