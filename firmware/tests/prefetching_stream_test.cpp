#include "media/storage/prefetching_stream.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "check failed: %s\n", what);
    return condition;
}

// Stands in for the SD-backed stream.  Counts reads so the test can show that
// the decoder's many small reads turn into few large ones on the card.
class FakeSource final : public media::AudioStream {
public:
    explicit FakeSource(std::size_t size) : data_(size) {
        std::iota(data_.begin(), data_.end(), 0);
    }

    media::AudioStreamStatus read(uint8_t* destination, std::size_t capacity,
                                  std::size_t& bytes_read) noexcept override {
        ++reads;
        bytes_read = 0;
        if (fail) return media::AudioStreamStatus::IoError;
        if (position_ >= data_.size()) return media::AudioStreamStatus::Eof;
        bytes_read = std::min(capacity, data_.size() - position_);
        std::memcpy(destination, data_.data() + position_, bytes_read);
        position_ += bytes_read;
        return media::AudioStreamStatus::Ok;
    }

    media::AudioStreamStatus seek(uint64_t offset) noexcept override {
        if (offset > data_.size()) return media::AudioStreamStatus::OutOfRange;
        position_ = static_cast<std::size_t>(offset);
        return media::AudioStreamStatus::Ok;
    }
    uint64_t tell() const noexcept override { return position_; }
    uint64_t size() const noexcept override { return data_.size(); }
    bool is_open() const noexcept override { return open_; }
    media::AudioStreamStatus close() noexcept override {
        open_ = false;
        if (closed_out != nullptr) *closed_out = true;
        return media::AudioStreamStatus::Ok;
    }

    std::size_t reads = 0;
    bool fail = false;
    bool* closed_out = nullptr;

private:
    std::vector<uint8_t> data_;
    std::size_t position_ = 0;
    bool open_ = true;
};

bool test_prefetch_serves_reads_from_the_ring() {
    auto source = std::make_unique<FakeSource>(4096);
    auto* raw = source.get();
    media::PrefetchingStream stream(std::move(source), 2048, 512);

    // Host build has no task, so filling is driven explicitly.
    for (int i = 0; i < 4; ++i) stream.fill_once();
    if (!check(stream.buffered() == 2048, "the ring fills to capacity")) return false;
    if (!check(raw->reads == 4, "the ring is filled in chunks")) return false;

    uint8_t out[16] = {};
    std::size_t got = 0;
    if (!check(stream.read(out, 16, got) == media::AudioStreamStatus::Ok && got == 16,
               "a small read is served")) return false;
    if (!check(raw->reads == 4, "a buffered read does not touch the source")) return false;
    for (uint8_t i = 0; i < 16; ++i) {
        if (!check(out[i] == i, "buffered bytes are in order")) return false;
    }
    return check(stream.tell() == 16, "position follows delivered bytes");
}

bool test_data_survives_the_whole_file() {
    auto source = std::make_unique<FakeSource>(3000);
    media::PrefetchingStream stream(std::move(source), 1024, 256);

    std::vector<uint8_t> collected;
    uint8_t out[128] = {};
    std::size_t got = 0;
    while (true) {
        stream.fill_once();
        const auto status = stream.read(out, sizeof(out), got);
        if (status != media::AudioStreamStatus::Ok || got == 0) break;
        collected.insert(collected.end(), out, out + got);
    }

    if (!check(collected.size() == 3000, "every byte is delivered")) return false;
    for (std::size_t i = 0; i < collected.size(); ++i) {
        if (collected[i] != static_cast<uint8_t>(i)) {
            return check(false, "bytes are delivered in order");
        }
    }
    return true;
}

bool test_eof_is_reported_after_the_ring_drains() {
    auto source = std::make_unique<FakeSource>(100);
    media::PrefetchingStream stream(std::move(source), 512, 256);
    while (stream.fill_once()) {
    }

    uint8_t out[256] = {};
    std::size_t got = 0;
    return check(stream.read(out, sizeof(out), got) == media::AudioStreamStatus::Ok && got == 100,
                 "the remaining bytes come out") &&
           check(stream.source_exhausted(), "the source is marked exhausted") &&
           check(stream.read(out, sizeof(out), got) == media::AudioStreamStatus::Eof && got == 0,
                 "EOF follows once the ring is empty");
}

bool test_source_errors_surface() {
    auto source = std::make_unique<FakeSource>(1024);
    auto* raw = source.get();
    raw->fail = true;
    media::PrefetchingStream stream(std::move(source), 512, 256);
    stream.fill_once();

    uint8_t out[8] = {};
    std::size_t got = 0;
    return check(stream.read(out, sizeof(out), got) == media::AudioStreamStatus::IoError,
                 "a source failure is reported, not silently treated as EOF");
}

bool test_seek_discards_buffered_data() {
    auto source = std::make_unique<FakeSource>(4096);
    media::PrefetchingStream stream(std::move(source), 1024, 512);
    stream.fill_once();
    stream.fill_once();

    if (!check(stream.seek(2000) == media::AudioStreamStatus::Ok, "seek succeeds")) return false;
    if (!check(stream.buffered() == 0, "seek drops data from the old position")) return false;
    if (!check(stream.tell() == 2000, "seek moves the position")) return false;

    stream.fill_once();
    uint8_t out[4] = {};
    std::size_t got = 0;
    stream.read(out, sizeof(out), got);
    return check(got == 4 && out[0] == static_cast<uint8_t>(2000),
                 "data after a seek comes from the new position");
}

bool test_reading_survives_repeated_seeks() {
    // Skipping an ID3 tag seeks before the first read, so a seek must leave the
    // stream fully usable.  On device a seek also stops and restarts the fill
    // task; forgetting the restart left the decoder waiting forever.
    auto source = std::make_unique<FakeSource>(4096);
    media::PrefetchingStream stream(std::move(source), 1024, 256);

    for (uint64_t offset : {uint64_t{100}, uint64_t{2000}, uint64_t{50}}) {
        if (!check(stream.seek(offset) == media::AudioStreamStatus::Ok, "seek succeeds")) return false;
        stream.fill_once();
        uint8_t out[8] = {};
        std::size_t got = 0;
        if (!check(stream.read(out, sizeof(out), got) == media::AudioStreamStatus::Ok && got == 8,
                   "data is still readable after a seek")) return false;
        if (!check(out[0] == static_cast<uint8_t>(offset), "data comes from the new position")) {
            return false;
        }
    }
    return true;
}

bool test_close_releases_the_source() {
    bool closed = false;
    auto source = std::make_unique<FakeSource>(64);
    source->closed_out = &closed;
    {
        media::PrefetchingStream stream(std::move(source), 256, 64);
        stream.close();
    }
    return check(closed, "closing the prefetcher closes the source");
}

}  // namespace

int main() {
    const bool ok = test_prefetch_serves_reads_from_the_ring() &&
                    test_data_survives_the_whole_file() &&
                    test_eof_is_reported_after_the_ring_drains() && test_source_errors_surface() &&
                    test_seek_discards_buffered_data() && test_reading_survives_repeated_seeks() &&
                    test_close_releases_the_source();
    return ok ? 0 : 1;
}
