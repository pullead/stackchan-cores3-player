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

bool test_rejects_non_track_names() {
    return check(!media::is_supported_wav_filename("track"), "no extension is rejected") &&
           check(!media::is_supported_wav_filename("track.wave"), ".wave is rejected") &&
           check(!media::is_supported_wav_filename("albums.wav/"), "directory-looking name is rejected") &&
           check(!media::is_supported_wav_filename("track."), "trailing dot is rejected");
}

struct FakeBrowseContext {
    std::vector<std::string> events;
    bool drain_succeeds = true;
    bool mount_succeeds = true;
    bool list_succeeds = true;
    bool unmount_succeeds = true;
    int cs_failures_remaining = 0;
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
    if (context.cs_failures_remaining > 0) {
        --context.cs_failures_remaining;
        error = "chip select rejected";
        return false;
    }
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
                                      set_sd_cs_high, unlock_display});
}

media::SdCardOperations make_sd_operations(FakeBrowseContext& context) {
    return {&context, mount_sd, list_sd, unmount_sd};
}

bool test_browse_is_one_atomic_handoff_transaction() {
    FakeBrowseContext context;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.size() == 1, "browse returns the listed track") &&
           check(port.last_error().empty(), "successful browse clears the error") &&
           check(events_equal(context.events,
                              {"lock", "drain", "input", "mount", "list", "unmount", "cs_high", "unlock"}),
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
           check(events_equal(context.events, {"lock", "drain", "input", "mount", "cs_high", "unlock"}),
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
                              {"lock", "drain", "input", "mount", "list", "unmount", "cs_high", "unlock"}),
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
                              {"lock", "drain", "input", "mount", "list", "unmount", "cs_high", "unlock"}),
                 "unmount failure still deselects SD before unlocking display");
}

bool test_release_failure_discards_tracks_and_guard_retries_before_unlock() {
    FakeBrowseContext context;
    context.cs_failures_remaining = 1;
    auto handoff = make_handoff(context);
    media::SdCardPort port(handoff, make_sd_operations(context));

    const auto tracks = port.browse_tracks();

    return check(tracks.empty(), "failed release discards listed tracks") &&
           check(port.last_error() == "chip select rejected", "release error is preserved") &&
           check(events_equal(context.events,
                              {"lock", "drain", "input", "mount", "list", "unmount", "cs_high", "cs_high", "unlock"}),
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
    failures += !test_rejects_non_track_names();
    failures += !test_browse_is_one_atomic_handoff_transaction();
    failures += !test_handoff_failure_never_touches_sd();
    failures += !test_mount_failure_releases_without_listing_or_unmounting();
    failures += !test_list_failure_still_unmounts_and_releases();
    failures += !test_unmount_failure_clears_tracks_and_releases();
    failures += !test_release_failure_discards_tracks_and_guard_retries_before_unlock();
    failures += !test_default_host_port_reports_hardware_only();
    return failures == 0 ? 0 : 1;
}
