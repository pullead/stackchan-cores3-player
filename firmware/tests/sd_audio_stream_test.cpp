#include "media/storage/sd_audio_stream.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Fake { std::string data = "abcdef"; bool mounted = false; bool closed = false; };
bool mount(void* c, std::string&) { static_cast<Fake*>(c)->mounted = true; return true; }
bool open(void* c, std::string_view path, void*& h, uint64_t& size, std::string&) {
    if (path != "/sdcard/a.mp3") return false; h = c; size = static_cast<Fake*>(c)->data.size(); return true;
}
std::size_t read(void*, void* h, uint8_t* out, std::size_t n, std::string&) {
    auto* f = static_cast<Fake*>(h); static std::size_t p = 0; const auto count = (p < f->data.size()) ? (f->data.size() - p < n ? f->data.size() - p : n) : 0; std::memcpy(out, f->data.data() + p, count); p += count; return count;
}
bool seek(void*, void*, uint64_t, std::string&) { return true; }
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
    stream.close(); return fake.mounted || !fake.closed ? 3 : 0;
}
