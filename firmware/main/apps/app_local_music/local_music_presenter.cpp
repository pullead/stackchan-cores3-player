#include "local_music_presenter.h"

#include <algorithm>

namespace local_music {

BrowseView make_browse_view(const std::vector<media::SdTrack>& tracks, const std::string& error) {
    BrowseView view;

    if (!error.empty()) {
        view.heading = "SD CARD ERROR";
        view.detail = error;
        return view;
    }

    if (tracks.empty()) {
        view.heading = "NO WAV FILES";
        view.detail = "FAT32 root folder";
        return view;
    }

    const std::size_t visible_count = std::min(tracks.size(), kVisibleTrackRows);
    if (tracks.size() > visible_count) {
        view.heading = std::to_string(visible_count) + " OF " + std::to_string(tracks.size()) + " WAV FILES";
    } else {
        view.heading = std::to_string(tracks.size()) + " WAV FILES";
    }
    view.detail = "BROWSE ONLY - MUTED";
    view.rows.reserve(visible_count);
    for (std::size_t index = 0; index < visible_count; ++index) {
        view.rows.push_back(tracks[index].title);
    }
    return view;
}

}  // namespace local_music
