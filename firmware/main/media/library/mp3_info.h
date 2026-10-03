#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace media {

// What an MP3 carries in front of its audio data.  An empty field means the tag
// did not have it, not that the file is broken; the caller keeps the filename
// in that case.
struct Mp3Tags {
    std::string title;
    std::string artist;
    std::string album;

    bool any() const {
        return !title.empty() || !artist.empty() || !album.empty();
    }
};

// Reads the ID3v2.3/2.4 text frames at the start of `data`.  `tag_bytes`
// reports where the audio data begins, so the caller can look for the first
// MPEG frame there.
//
// Returns false when there is nothing usable: no tag, an ID3v2.2 tag (its frame
// layout is different), an unsynchronised tag, a truncated buffer, or a
// malformed header.  All of those simply mean "show the filename".
bool parse_id3v2_tags(const std::uint8_t* data, std::size_t size, Mp3Tags& tags,
                      std::size_t& tag_bytes);

struct Mp3AudioInfo {
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 0;
    std::uint32_t bitrate_kbps = 0;
    // True when the duration came from a Xing/Info frame count rather than from
    // the bitrate, which is the only correct answer for a VBR file.
    bool from_xing = false;
    std::uint32_t duration_seconds = 0;
};

// Finds the first MPEG-1/2/2.5 Layer III frame at or after `start_offset` and
// derives the duration.  With a Xing/Info header the frame count is used;
// otherwise the duration is estimated from `file_bytes` and the frame bitrate,
// which is accurate for CBR and only an estimate for VBR.
bool parse_mp3_audio_info(const std::uint8_t* data, std::size_t size, std::size_t start_offset,
                          std::uint64_t file_bytes, Mp3AudioInfo& out);

}  // namespace media
