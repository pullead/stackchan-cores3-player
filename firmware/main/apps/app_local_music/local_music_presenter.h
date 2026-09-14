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

BrowseView make_browse_view(const std::vector<media::SdTrack>& tracks, const std::string& error);

}  // namespace local_music
