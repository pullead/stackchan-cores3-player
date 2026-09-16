#include "media/storage/sd_audio_stream.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Fake {
    std::string data = "abcdefghij";
    bool mounted = false;
    bool closed = false;
    std::size_t position = 0;
    std::size_t reads = 0;
    bool read_error = false;
    bool close_error = false;
    bool unmount_error = false;
};

bool mount(void* c, std::string&) { static_cast<Fake*>(c)->mounted = true; return true; }
bool open(void* c, std::string_view path, void*& h, uint64_t& size, std::string&) {
    if (path != "/sdcard/a.mp3") return false;
    h = c; size = static_cast<Fake*>(c)->data.size(); return true;
}
std::size_t read(void*, void* h, uint8_t* out, std::size_t n, bool& io_error, std::string& error) {
    auto* f = static_cast<Fake*>(h);
    ++f->reads;
    io_error = f->read_error;
    if (io_error) { error = "fake read error"; return 0; }
    const auto available = (f->position < f->data.size()) ? f->data.size() - f->position : 0;
    const auto count = available < n ? available : n;
    std::memcpy(out, f->data.data() + f->position, count);
    f->position += count;
    return count;
}
bool seek(void*, void* h, uint64_t offset, std::string&) {
    auto* f = static_cast<Fake*>(h);
    if (offset > f->data.size()) return false;
    f->position = static_cast<std::size_t>(offset);
    return true;
}
bool close(void* c, void*, std::string& error) {
    auto* f = static_cast<Fake*>(c); f->closed = true;
    if (f->close_error) { error = "fake close error"; return false; }
    return true;
}
bool unmount(void* c, std::string& error) {
    auto* f = static_cast<Fake*>(c);
    if (f->unmount_error) { error = "fake unmount error"; return false; }
    f->mounted = false; return true;
}
bool lock(void*, std::string&) { return true; }
bool drain(void*, std::string&) { return true; }
bool pin(void*, std::string&) { return true; }
void unlock(void*) {}

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "check failed: %s\n", what);
    return condition;
}
}  // namespace

int main() {
    board::Spi3DisplayHandoffOperations hop{nullptr, lock, drain, pin, pin, pin, unlock};
    board::Spi3DisplayHandoff handoff(hop);

    // An open track must not keep the display bus: the screen has to stay
    // usable while music plays.
    {
        Fake fake;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 4);
        if (!check(stream.is_open() && stream.size() == 10, "stream opens")) return 1;
        if (!check(!handoff.is_acquired(), "an open stream does not hold the display bus")) return 1;

        uint8_t out[3]{};
        std::size_t got = 0;
        const std::size_t borrows_before = stream.borrow_count();
        if (!check(stream.read(out, 3, got) == media::AudioStreamStatus::Ok && got == 3 &&
                       std::memcmp(out, "abc", 3) == 0,
                   "first read returns data")) return 2;
        if (!check(!handoff.is_acquired(), "the bus is handed back after a read")) return 2;
        if (!check(stream.read(out, 3, got) == media::AudioStreamStatus::Ok && got == 3 &&
                       std::memcmp(out, "def", 3) == 0,
                   "second read crosses the prefetch boundary")) return 3;
        // Six bytes delivered from a 4-byte prefetch needs two refills; the
        // point is that borrows track prefetches, not decoder reads.
        if (!check(stream.borrow_count() - borrows_before <= 2,
                   "six bytes come from at most two borrows")) return 3;
        if (!check(fake.reads <= 2, "small reads are coalesced into prefetches")) return 3;
        if (!check(stream.tell() == 6, "position follows delivered bytes, not prefetched ones"))
            return 3;
    }

    // Reading to the end reports EOF once the buffer is drained.
    {
        Fake fake;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 4);
        uint8_t out[16]{};
        std::size_t got = 0;
        if (!check(stream.read(out, 16, got) == media::AudioStreamStatus::Ok && got == 10 &&
                       std::memcmp(out, "abcdefghij", 10) == 0,
                   "a large read drains the file")) return 4;
        if (!check(stream.read(out, 4, got) == media::AudioStreamStatus::Eof && got == 0,
                   "the next read reports EOF")) return 4;
    }

    // Seeking discards buffered bytes from the old position.
    {
        Fake fake;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 8);
        uint8_t out[2]{};
        std::size_t got = 0;
        stream.read(out, 2, got);
        if (!check(stream.seek(1) == media::AudioStreamStatus::Ok && stream.tell() == 1,
                   "seek moves the logical position")) return 5;
        if (!check(stream.read(out, 2, got) == media::AudioStreamStatus::Ok && got == 2 &&
                       std::memcmp(out, "bc", 2) == 0,
                   "data after a seek comes from the new position")) return 5;
        if (!check(stream.seek(99) == media::AudioStreamStatus::OutOfRange,
                   "seeking past the end is rejected")) return 5;
    }

    // Closing releases the card and the bus.
    {
        Fake fake;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 8);
        stream.close();
        if (!check(stream.close() == media::AudioStreamStatus::Ok, "closing twice is harmless")) return 6;
        if (!check(!fake.mounted && fake.closed && !handoff.is_acquired(),
                   "close unmounts and releases the bus")) return 6;
    }

    // Failure paths are reported, not swallowed.  Each case gets its own scope:
    // one handoff cannot be borrowed twice at the same time.
    {
        Fake fake; fake.read_error = true;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 8);
        uint8_t out[3]{}; std::size_t got = 0;
        if (!check(stream.read(out, 3, got) == media::AudioStreamStatus::IoError &&
                       stream.last_error() == "fake read error",
                   "read errors propagate")) return 7;
    }
    {
        Fake fake; fake.close_error = true;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 8);
        if (!check(stream.is_open(), "close-error fixture opens")) return 8;
        if (!check(stream.close() == media::AudioStreamStatus::IoError &&
                       stream.last_error() == "fake close error" && !stream.is_open(),
                   "close errors propagate")) return 8;
    }
    {
        Fake fake; fake.unmount_error = true;
        media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
        media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3", 8);
        if (!check(stream.is_open(), "unmount-error fixture opens")) return 9;
        if (!check(stream.close() == media::AudioStreamStatus::IoError &&
                       stream.last_error() == "fake unmount error" && !handoff.is_acquired(),
                   "unmount errors propagate and still release the bus")) return 9;
    }
    return 0;
}
