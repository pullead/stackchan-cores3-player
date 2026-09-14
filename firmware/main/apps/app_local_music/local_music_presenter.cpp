#include "local_music_presenter.h"

namespace local_music {

BrowseView make_browse_view(const std::vector<media::SdTrack>& tracks, const std::string& error) {
    BrowseView view;

    if (!error.empty()) {
        view.heading = "SD CARD ERROR";
        view.detail = error == "ESP_FAIL" ? "FAT32 CARD REQUIRED / CHECK FILESYSTEM" : error;
        return view;
    }

    if (tracks.empty()) {
        view.heading = "NO AUDIO FILES";
        view.detail = "FAT32 ROOT / AUDIOFILES";
        return view;
    }

    view.heading = std::to_string(tracks.size()) + " AUDIO FILES";
    view.detail = "BROWSE ONLY - MUTED";
    view.rows.reserve(tracks.size());
    for (const auto& track : tracks) {
        view.rows.push_back(track.title);
    }
    return view;
}

}  // namespace local_music
