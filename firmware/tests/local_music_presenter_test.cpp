#include "apps/app_local_music/local_music_presenter.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

bool test_reports_sd_error_without_rows() {
    const auto view = local_music::make_browse_view({}, "ESP_ERR_NOT_FOUND");
    return check(view.heading == "SD CARD ERROR", "error heading is explicit") &&
           check(view.detail == "ESP_ERR_NOT_FOUND", "hardware error remains visible") &&
           check(view.rows.empty(), "error state has no stale rows");
}

bool test_explains_fat_mount_failure() {
    const auto view = local_music::make_browse_view({}, "ESP_FAIL");
    return check(view.heading == "SD CARD ERROR", "mount error heading remains explicit") &&
           check(view.detail == "FAT32 CARD REQUIRED / CHECK FILESYSTEM", "generic mount error is actionable") &&
           check(view.rows.empty(), "mount error has no stale rows");
}

bool test_reports_empty_card() {
    const auto view = local_music::make_browse_view({}, "");
    return check(view.heading == "NO WAV FILES", "empty state is explicit") &&
           check(view.detail == "FAT32 root folder", "empty state explains scan scope") &&
           check(view.rows.empty(), "empty state has no rows");
}

bool test_limits_rows_and_preserves_total_count() {
    std::vector<media::SdTrack> tracks;
    for (int index = 0; index < 10; ++index) {
        tracks.push_back({"/sdcard/track.wav", "Track " + std::to_string(index + 1), 48});
    }

    const auto view = local_music::make_browse_view(tracks, "");
    return check(view.heading == "8 OF 10 WAV FILES", "status reports truncation") &&
           check(view.detail == "BROWSE ONLY - MUTED", "silent browse gate is visible") &&
           check(view.rows.size() == 8, "small-screen list is bounded") &&
           check(view.rows.front() == "Track 1", "first title is preserved") &&
           check(view.rows.back() == "Track 8", "eighth title is preserved");
}

bool test_reports_all_visible_tracks() {
    const std::vector<media::SdTrack> tracks = {
        {"/sdcard/a.wav", "A", 48},
        {"/sdcard/b.wav", "B", 96},
    };
    const auto view = local_music::make_browse_view(tracks, "");
    return check(view.heading == "2 WAV FILES", "status reports complete count") &&
           check(view.rows == std::vector<std::string>({"A", "B"}), "all titles are shown");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_reports_sd_error_without_rows();
    failures += !test_explains_fat_mount_failure();
    failures += !test_reports_empty_card();
    failures += !test_limits_rows_and_preserves_total_count();
    failures += !test_reports_all_visible_tracks();
    return failures == 0 ? 0 : 1;
}
