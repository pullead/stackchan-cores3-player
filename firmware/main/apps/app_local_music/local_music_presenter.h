#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <media/storage/sd_card_port.h>

namespace local_music {

inline constexpr std::size_t kVisibleTrackRows = 8;

struct BrowseView {
    std::string heading;
    std::string detail;
    std::vector<std::string> rows;
};

// UI-facing selection result.  Keeping this decision independent of LVGL
// makes it testable and prevents a missing decoder from looking like playback.
struct SelectionResult {
    bool accepted = false;
    std::size_t index = 0;
    std::string title;
    std::string detail;
};

BrowseView make_browse_view(const std::vector<media::SdTrack>& tracks, const std::string& error);
SelectionResult select_track(const std::vector<media::SdTrack>& tracks, std::size_t index);
BrowseView make_decoder_unavailable_view(const media::SdTrack& track);

}  // namespace local_music
