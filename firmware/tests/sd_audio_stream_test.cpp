#include "media/storage/sd_audio_stream.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Fake { std::string data = "abcdef"; bool mounted = false; bool closed = false; std::size_t position = 0; };
bool mount(void* c, std::string&) { static_cast<Fake*>(c)->mounted = true; return true; }
bool open(void* c, std::string_view path, void*& h, uint64_t& size, std::string&) {
    if (path != "/sdcard/a.mp3") return false; h = c; size = static_cast<Fake*>(c)->data.size(); return true;
}
std::size_t read(void*, void* h, uint8_t* out, std::size_t n, std::string&) {
    auto* f = static_cast<Fake*>(h); const auto count = (f->position < f->data.size()) ? (f->data.size() - f->position < n ? f->data.size() - f->position : n) : 0; std::memcpy(out, f->data.data() + f->position, count); f->position += count; return count;
}
bool seek(void*, void* h, uint64_t offset, std::string&) { auto* f = static_cast<Fake*>(h); if (offset > f->data.size()) return false; f->position = static_cast<std::size_t>(offset); return true; }
void close(void* c, void*) { static_cast<Fake*>(c)->closed = true; }
bool unmount(void* c, std::string&) { static_cast<Fake*>(c)->mounted = false; return true; }
bool lock(void*, std::string&) { return true; } bool drain(void*, std::string&) { return true; } bool pin(void*, std::string&) { return true; } void unlock(void*) {}
}
int main() {
    board::Spi3DisplayHandoffOperations hop{nullptr, lock, drain, pin, pin, pin, unlock};
    board::Spi3DisplayHandoff handoff(hop); Fake fake;
    media::SdAudioFileOperations ops{&fake, mount, open, read, seek, close, unmount};
    media::SdAudioStream stream(handoff, ops, "/sdcard/a.mp3");
    if (!stream.is_open() || stream.size() != 6) return 1;
    uint8_t out[3]{}; std::size_t got = 0;
    if (stream.read(out, sizeof(out), got) != media::AudioStreamStatus::Ok || got != 3 || std::memcmp(out, "abc", 3) != 0) return 2;
    if (stream.tell() != 3 || stream.seek(1) != media::AudioStreamStatus::Ok || stream.tell() != 1) return 3;
    if (stream.seek(99) != media::AudioStreamStatus::OutOfRange) return 4;
    stream.close(); return fake.mounted || !fake.closed || handoff.is_acquired() ? 5 : 0;
}
