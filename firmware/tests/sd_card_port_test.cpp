#include "media/storage/sd_card_port.h"
#include "hal/board/spi3_display_handoff.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

bool test_accepts_common_local_audio_extensions() {
    return check(media::is_supported_audio_filename("track.mp3"), ".mp3 is accepted") &&
           check(media::is_supported_audio_filename("track.AAC"), ".aac is accepted") &&
           check(media::is_supported_audio_filename("track.m4a"), ".m4a is accepted") &&
           check(media::is_supported_audio_filename("track.flac"), ".flac is accepted") &&
           check(media::is_supported_audio_filename("track.ogg"), ".ogg is accepted") &&
           check(media::is_supported_audio_filename("track.opus"), ".opus is accepted");
}

bool test_rejects_non_track_names() {
    return check(!media::is_supported_wav_filename("track"), "no extension is rejected") &&
           check(!media::is_supported_wav_filename("track.wave"), ".wave is rejected") &&
           check(!media::is_supported_wav_filename("albums.wav/"), "directory-looking name is rejected") &&
           check(!media::is_supported_wav_filename("track."), "trailing dot is rejected");
}

bool test_rejects_lyrics_and_directories() {
    return check(!media::is_supported_audio_filename("track.lrc"), "lyrics are rejected") &&
           check(!media::is_supported_audio_filename("audiofiles/"), "directories are rejected");
}

struct FakeBrowseContext {
    std::vector<std::string> events;
    bool drain_succeeds = true;
    bool mount_succeeds = true;
    bool list_succeeds = true;
    bool unmount_succeeds = true;
    int cs_failures_remaining = 0;
    int cs_call_count = 0;
    int cs_failure_start_call = 0;
};

bool lock_display(void* raw_context, std::string&) {
    static_cast<FakeBrowseContext*>(raw_context)->events.emplace_back("lock");
    return true;
}

bool drain_display(void* raw_context, std::string& error) {
    auto& context = *static_cast<FakeBrowseContext*>(raw_context);
    context.events.emplace_back("drain");
    if (!context.drain_succeeds) {
        error = "display drain rejected";
        return false;
    }
    return true;
}

bool set_shared_pin_input(void* raw_context, std::string&) {
    static_cast<FakeBrowseContext*>(raw_context)->events.emplace_back("input");
    return true;
}

bool set_sd_cs_high(void* raw_context, std::string& error) {
    auto& context = *static_cast<FakeBrowseContext*>(raw_context);
    context.events.emplace_back("cs_high");
    ++context.cs_call_count;
    if (context.cs_call_count >= context.cs_failure_start_call && context.cs_failures_remaining > 0) {
        --context.cs_failures_remaining;
        error = "chip select rejected";
        return false;
    }
    return true;
}

bool restore_shared_pin_display_output(void* raw_context, std::string&) {
    static_cast<FakeBrowseContext*>(raw_context)->events.emplace_back("restore_output");
    return true;
}

void unlock_display(void* raw_context) {
    static_cast<FakeBrowseContext*>(raw_context)->events.emplace_back("unlock");
}

bool mount_sd(void* raw_context, std::string& error) {
    auto& context = *static_cast<FakeBrowseContext*>(raw_context);
    context.events.emplace_back("mount");
    if (!context.mount_succeeds) {
        error = "mount rejected";
        return false;
    }
    return true;
}

bool list_sd(void* raw_context, std::vector<media::SdTrack>& tracks, std::string& error) {
    auto& context = *static_cast<FakeBrowseContext*>(raw_context);
    context.events.emplace_back("list");
    if (!context.list_succeeds) {
        error = "directory read rejected";
        return false;
    }
    tracks.push_back({"/sdcard/example.wav", "example", 48});
    return true;
}

bool unmount_sd(void* raw_context, std::string& error) {
    auto& context = *static_cast<FakeBrowseContext*>(raw_context);
    context.events.emplace_back("unmount");
    if (!context.unmount_succeeds) {
        error = "unmount rejected";
        return false;
    }
    return true;
}

bool events_equal(const std::vector<std::string>& actual, std::initializer_list<const char*> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    size_t index = 0;
    for (const char* event : expected) {
        if (actual[index++] != event) {
            return false;
        }
    }
    return true;
}

board::Spi3DisplayHandoff make_handoff(FakeBrowseContext& context) {
    return board::Spi3DisplayHandoff({&context, lock_display, drain_display, set_shared_pin_input,
                                      set_sd_cs_high, restore_shared_pin_display_output, unlock_display});
}

media::SdCardOperations make_sd_operations(FakeBrowseContext& context) {
    return {&context, mount_sd, list_sd, unmount_sd};
}

struct HeadContext {
    std::vector<uint8_t> bytes;
    std::string last_path;
    bool fail = false;
};

bool read_head_fake(void* context, std::string_view path, std::vector<uint8_t>& buffer,
                    std::size_t max_bytes, std::string& error) {
    auto* head = static_cast<HeadContext*>(context);
    head->last_path = std::string(path);
    if (head->fail) {
        error = "head read rejected";
        return false;
    }
    const std::size_t count = max_bytes < head->bytes.size() ? max_bytes : head->bytes.size();
    buffer = head->bytes;
    buffer.resize(count);
    return true;
}

bool test_read_head_delegates_to_the_operation() {
    HeadContext head;
    head.bytes = {1, 2, 3, 4, 5};
    FakeBrowseContext browse;
    auto handoff = make_handoff(browse);
    media::SdCardPort port(handoff, media::SdCardOperations{&head, nullptr, nullptr, nullptr, read_head_fake});

    const media::SdTrack track{"audiofiles/one.mp3", "one", 5};
    std::vector<uint8_t> buffer;
    if (!check(port.read_head(track, buffer, 3), "head read succeeds") ||
        !check(buffer == std::vector<uint8_t>{1, 2, 3}, "the caller's limit is honoured") ||
        !check(head.last_path == "audiofiles/one.mp3", "the track path is passed through")) {
        return false;
    }

    // A file shorter than the probe returns what it has rather than padding.
    if (!check(port.read_head(track, buffer, 99), "a short file still reads") ||
        !check(buffer.size() == 5, "and returns only its own bytes")) {
        return false;
    }

    head.fail = true;
    return check(!port.read_head(track, buffer, 3), "a failed read is reported") &&
           check(buffer.empty(), "and leaves no partial bytes behind") &&
           check(port.last_error() == "head read rejected", "and the operation's error is preserved");
}

bool test_read_head_without_an_operation_is_a_clear_failure() {
    media::SdCardPort port;
    const media::SdTrack track{"audiofiles/one.mp3", "one", 0};
    std::vector<uint8_t> buffer;
    return check(!port.read_head(track, buffer, 8), "a port without a head reader fails") &&
           check(port.last_error() == "SD head reads are not available", "and names the reason");
}

bool test_browse_is_one_atomic_handoff_transaction() {
    FakeBrowseContext context;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.size() == 1, "browse returns the listed track") &&
           check(port.last_error().empty(), "successful browse clears the error") &&
           check(events_equal(context.events,
                              {"lock", "drain", "cs_high", "input", "mount", "list", "unmount", "cs_high", "restore_output", "unlock"}),
                 "mount, list, and unmount stay inside one handoff");
}

bool test_handoff_failure_never_touches_sd() {
    FakeBrowseContext context;
    context.drain_succeeds = false;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed handoff returns no tracks") &&
           check(port.last_error() == "display drain rejected", "handoff error is preserved") &&
           check(events_equal(context.events, {"lock", "drain", "unlock"}),
                 "failed handoff performs no SD operation");
}

bool test_mount_failure_releases_without_listing_or_unmounting() {
    FakeBrowseContext context;
    context.mount_succeeds = false;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed mount returns no tracks") &&
           check(port.last_error() == "mount rejected", "mount error is preserved") &&
           check(events_equal(context.events, {"lock", "drain", "cs_high", "input", "mount", "cs_high", "restore_output", "unlock"}),
                 "mount failure deselects SD and unlocks without list or unmount");
}

bool test_list_failure_still_unmounts_and_releases() {
    FakeBrowseContext context;
    context.list_succeeds = false;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed directory read returns no partial tracks") &&
           check(port.last_error() == "directory read rejected", "directory read error is preserved") &&
           check(events_equal(context.events,
                              {"lock", "drain", "cs_high", "input", "mount", "list", "unmount", "cs_high", "restore_output", "unlock"}),
                 "list error still unmounts before releasing display");
}

bool test_unmount_failure_clears_tracks_and_releases() {
    FakeBrowseContext context;
    context.unmount_succeeds = false;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed unmount discards listed tracks") &&
           check(port.last_error() == "unmount rejected", "unmount error is preserved") &&
           check(events_equal(context.events,
                              {"lock", "drain", "cs_high", "input", "mount", "list", "unmount", "cs_high", "restore_output", "unlock"}),
                 "unmount failure still deselects SD before unlocking display");
}

bool test_release_failure_discards_tracks_and_guard_retries_before_unlock() {
    FakeBrowseContext context;
    // Let acquisition complete, then fail the first release deselect so the
    // guard's destructor must retry before restoring the display pin.
    context.cs_failures_remaining = 1;
    context.cs_failure_start_call = 2;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed release discards listed tracks") &&
           check(port.last_error() == "chip select rejected", "release error is preserved") &&
           check(events_equal(context.events,
                              {"lock", "drain", "cs_high", "input", "mount", "list", "unmount", "cs_high", "cs_high", "restore_output", "unlock"}),
                 "guard retries deselect and only then unlocks display");
}

bool test_default_host_port_reports_hardware_only() {
    media::SdCardPort port;

    return check(port.browse_tracks().empty(), "host browse returns no tracks") &&
           check(port.last_error() == "SD card is only available on ESP hardware",
                 "host browse reports the hardware boundary");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_accepts_wav_extension_case_insensitively();
    failures += !test_accepts_common_local_audio_extensions();
    failures += !test_rejects_non_track_names();
    failures += !test_rejects_lyrics_and_directories();
    failures += !test_browse_is_one_atomic_handoff_transaction();
    failures += !test_handoff_failure_never_touches_sd();
    failures += !test_mount_failure_releases_without_listing_or_unmounting();
    failures += !test_list_failure_still_unmounts_and_releases();
    failures += !test_unmount_failure_clears_tracks_and_releases();
    failures += !test_release_failure_discards_tracks_and_guard_retries_before_unlock();
    failures += !test_default_host_port_reports_hardware_only();
    failures += !test_read_head_delegates_to_the_operation();
    failures += !test_read_head_without_an_operation_is_a_clear_failure();
    return failures == 0 ? 0 : 1;
}
