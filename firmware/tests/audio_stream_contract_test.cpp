#include "media/decoder/audio_decoder.h"

#include <cassert>
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
class FakeStream final : public media::AudioStream {
public:
    explicit FakeStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    media::AudioStreamStatus read(uint8_t* dst, size_t capacity, size_t& count) noexcept override {
        if (!dst || capacity == 0) { count = 0; return media::AudioStreamStatus::InvalidArgument; }
        count = std::min(capacity, bytes_.size() - position_);
        std::copy_n(bytes_.data() + position_, count, dst);
        position_ += count;
        return count == 0 ? media::AudioStreamStatus::Eof : media::AudioStreamStatus::Ok;
    }
    media::AudioStreamStatus seek(uint64_t offset) noexcept override {
        if (offset > bytes_.size()) return media::AudioStreamStatus::OutOfRange;
        position_ = static_cast<size_t>(offset); return media::AudioStreamStatus::Ok;
    }
    uint64_t tell() const noexcept override { return position_; }
    uint64_t size() const noexcept override { return bytes_.size(); }
    media::AudioStreamStatus close() noexcept override { closed_ = true; return media::AudioStreamStatus::Ok; }
private:
    std::vector<uint8_t> bytes_; size_t position_ = 0; bool closed_ = false;
};
}

int main() {
    FakeStream stream({1, 2, 3});
    uint8_t buffer[2]{}; size_t count = 0;
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Ok);
    assert(count == 2 && buffer[0] == 1 && buffer[1] == 2);
    assert(stream.tell() == 2);
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Ok && count == 1);
    assert(stream.read(buffer, sizeof(buffer), count) == media::AudioStreamStatus::Eof && count == 0);
    assert(stream.seek(1) == media::AudioStreamStatus::Ok && stream.tell() == 1);
    assert(stream.seek(4) == media::AudioStreamStatus::OutOfRange);
}
