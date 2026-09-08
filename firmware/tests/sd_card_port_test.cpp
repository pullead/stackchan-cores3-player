#include "media/storage/sd_card_port.h"

#include <cstdio>

namespace {

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

bool test_accepts_wav_extension_case_insensitively() {
    return check(media::is_supported_wav_filename("track.wav"), "lowercase .wav is accepted") &&
           check(media::is_supported_wav_filename("TRACK.WAV"), "uppercase .WAV is accepted") &&
           check(media::is_supported_wav_filename("MiXeD.WaV"), "mixed-case .WaV is accepted");
}

bool test_rejects_non_track_names() {
    return check(!media::is_supported_wav_filename("track"), "no extension is rejected") &&
           check(!media::is_supported_wav_filename("track.wave"), ".wave is rejected") &&
           check(!media::is_supported_wav_filename("albums.wav/"), "directory-looking name is rejected") &&
           check(!media::is_supported_wav_filename("track."), "trailing dot is rejected");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_accepts_wav_extension_case_insensitively();
    failures += !test_rejects_non_track_names();
    return failures == 0 ? 0 : 1;
}
