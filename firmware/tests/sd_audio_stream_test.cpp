#include "media/storage/sd_audio_stream.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Fake { std::string data = "abcdef"; bool mounted = false; bool closed = false; std::size_t position = 0; bool read_error = false; bool close_error = false; bool unmount_error = false; };
bool mount(void* c, std::string&) { static_cast<Fake*>(c)->mounted = true; return true; }
bool open(void* c, std::string_view path, void*& h, uint64_t& size, std::string&) {
    if (path != "/sdcard/a.mp3") return false; h = c; size = static_cast<Fake*>(c)->data.size(); return true;
}
std::size_t read(void*, void* h, uint8_t* out, std::size_t n, bool& io_error, std::string& error) {
    auto* f = static_cast<Fake*>(h); io_error = f->read_error; if (io_error) error = "fake read error";
    const auto available = (f->position < f->data.size()) ? f->data.size() - f->position : 0;
    const auto count = f->read_error ? (available > 0 ? std::size_t{1} : 0) : (available < n ? available : n);
    std::memcpy(out, f->data.data() + f->position, count); f->position += count; return count;
}
bool seek(void*, void* h, uint64_t offset, std::string&) { auto* f = static_cast<Fake*>(h); if (offset > f->data.size()) return false; f->position = static_cast<std::size_t>(offset); return true; }
bool close(void* c, void*, std::string& error) { auto* f = static_cast<Fake*>(c); f->closed = true; if (f->close_error) { error = "fake close error"; return false; } return true; }
bool unmount(void* c, std::string& error) { auto* f = static_cast<Fake*>(c); if (f->unmount_error) { error = "fake unmount error"; return false; } f->mounted = false; return true; }
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
    stream.close(); if (stream.close() != media::AudioStreamStatus::Ok || fake.mounted || !fake.closed || handoff.is_acquired()) return 5;
    fake = Fake{}; fake.read_error = true;
    media::SdAudioStream read_error_stream(handoff, ops, "/sdcard/a.mp3");
    if (!read_error_stream.is_open() || read_error_stream.read(out, sizeof(out), got) != media::AudioStreamStatus::IoError || read_error_stream.last_error() != "fake read error") return 6;
    fake = Fake{}; fake.close_error = true;
    media::SdAudioStream close_error_stream(handoff, ops, "/sdcard/a.mp3");
    if (close_error_stream.close() != media::AudioStreamStatus::IoError || close_error_stream.last_error() != "fake close error" || close_error_stream.is_open()) return 7;
    fake = Fake{}; fake.unmount_error = true;
    media::SdAudioStream unmount_error_stream(handoff, ops, "/sdcard/a.mp3");
    if (unmount_error_stream.close() != media::AudioStreamStatus::IoError || unmount_error_stream.last_error() != "fake unmount error" || handoff.is_acquired()) return 8;
    return 0;
}
