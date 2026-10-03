#include "media/library/mp3_info.h"

#include <algorithm>

namespace media {
namespace {

// ---------------------------------------------------------------- ID3v2 ----
//
// Header layout (ID3v2.3/v2.4): "ID3", major, minor, flags, then four bytes of
// synchsafe size that exclude the ten header bytes.

std::size_t synchsafe32(const std::uint8_t* bytes) {
    return (static_cast<std::size_t>(bytes[0]) << 21) |
           (static_cast<std::size_t>(bytes[1]) << 14) |
           (static_cast<std::size_t>(bytes[2]) << 7) | static_cast<std::size_t>(bytes[3]);
}

std::size_t big_endian32(const std::uint8_t* bytes) {
    return (static_cast<std::size_t>(bytes[0]) << 24) |
           (static_cast<std::size_t>(bytes[1]) << 16) |
           (static_cast<std::size_t>(bytes[2]) << 8) | static_cast<std::size_t>(bytes[3]);
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point == 0) return;
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

// UTF-16 to UTF-8, stopping at the first NUL unit.  Surrogate pairs are joined;
// an unpaired surrogate is dropped rather than emitted as mojibake.
void append_utf16(std::string& out, const std::uint8_t* data, std::size_t size, bool big_endian) {
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        const std::uint32_t unit =
            big_endian ? (static_cast<std::uint32_t>(data[i]) << 8) | data[i + 1]
                       : (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i];
        if (unit == 0) break;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 3 >= size) break;
            const std::uint32_t low =
                big_endian ? (static_cast<std::uint32_t>(data[i + 2]) << 8) | data[i + 3]
                           : (static_cast<std::uint32_t>(data[i + 3]) << 8) | data[i + 2];
            if (low < 0xDC00 || low > 0xDFFF) continue;
            append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            i += 2;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) continue;  // unpaired low surrogate
        append_utf8(out, unit);
    }
}

// One text frame body: the first byte picks the encoding, the rest is the text.
// Latin-1 is widened rather than copied, so a metadata byte above 0x7F cannot
// be mistaken for a UTF-8 continuation byte.
std::string decode_text_frame(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return {};
    const std::uint8_t encoding = data[0];
    const std::uint8_t* body = data + 1;
    std::size_t body_size = size - 1;

    if (encoding == 3) {  // UTF-8
        while (body_size > 0 && body[body_size - 1] == 0) --body_size;
        return std::string(reinterpret_cast<const char*>(body), body_size);
    }
    if (encoding == 0) {  // ISO-8859-1
        while (body_size > 0 && body[body_size - 1] == 0) --body_size;
        std::string out;
        out.reserve(body_size);
        for (std::size_t i = 0; i < body_size; ++i) {
            append_utf8(out, body[i]);
        }
        return out;
    }
    if (encoding == 1 || encoding == 2) {  // UTF-16, with and without a BOM
        bool big_endian = encoding == 2;
        if (encoding == 1) {
            if (body_size >= 2 && body[0] == 0xFE && body[1] == 0xFF) {
                body += 2;
                body_size -= 2;
                big_endian = true;
            } else if (body_size >= 2 && body[0] == 0xFF && body[1] == 0xFE) {
                body += 2;
                body_size -= 2;
                big_endian = false;
            } else {
                return {};  // encoding 1 without a BOM is ambiguous; do not guess
            }
        }
        std::string out;
        append_utf16(out, body, body_size, big_endian);
        return out;
    }
    return {};
}

// -------------------------------------------------------------- MPEG audio ---

struct FrameHeader {
    std::uint32_t sample_rate = 0;
    std::uint32_t bitrate_kbps = 0;
    std::uint8_t channels = 0;
    std::uint32_t samples_per_frame = 0;
    std::uint32_t length_bytes = 0;
    bool is_mpeg1 = false;
};

// Layer III only; the other layers' bitrate tables are deliberately absent.
bool decode_frame_header(std::uint32_t header, FrameHeader& out) {
    const std::uint32_t version_bits = (header >> 19) & 0x3;
    const std::uint32_t layer_bits = (header >> 17) & 0x3;
    const std::uint32_t bitrate_index = (header >> 12) & 0xF;
    const std::uint32_t rate_index = (header >> 10) & 0x3;
    const std::uint32_t padding = (header >> 9) & 0x1;
    const std::uint32_t channel_mode = (header >> 6) & 0x3;

    if (version_bits == 1 || layer_bits != 1) return false;  // reserved version / not Layer III
    if (bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return false;

    static const std::uint16_t kBitrateV1[16] = {0, 32, 40, 48, 56,  64,  80,  96,
                                                 112, 128, 160, 192, 224, 256, 320, 0};
    static const std::uint16_t kBitrateV2[16] = {0, 8,  16,  24,  32,  40,  48,  56,
                                                 64, 80, 96, 112, 128, 144, 160, 0};
    static const std::uint16_t kRateV1[3] = {44100, 48000, 32000};
    static const std::uint16_t kRateV2[3] = {22050, 24000, 16000};
    static const std::uint16_t kRateV25[3] = {11025, 12000, 8000};

    out.is_mpeg1 = version_bits == 3;
    out.bitrate_kbps = out.is_mpeg1 ? kBitrateV1[bitrate_index] : kBitrateV2[bitrate_index];
    out.sample_rate = out.is_mpeg1 ? kRateV1[rate_index]
                                   : (version_bits == 2 ? kRateV2[rate_index] : kRateV25[rate_index]);
    out.channels = channel_mode == 3 ? 1 : 2;
    // Layer III: MPEG-1 carries 1152 samples per frame, MPEG-2/2.5 carries 576.
    out.samples_per_frame = out.is_mpeg1 ? 1152 : 576;
    // Layer III frame length: 144 * bitrate / rate, or 72 * for MPEG-2/2.5.
    const std::uint32_t coefficient = out.is_mpeg1 ? 144 : 72;
    out.length_bytes = coefficient * out.bitrate_kbps * 1000 / out.sample_rate + padding;
    return out.bitrate_kbps > 0 && out.length_bytes > 4;
}

// Bytes between the frame header and the Xing/Info tag: the Layer III side
// information.  MPEG-1 is 32 bytes in stereo and 17 in mono; MPEG-2/2.5 is 17
// and 9.  Getting this wrong is the classic way to miss the tag entirely.
std::size_t side_info_bytes(const FrameHeader& frame) {
    const bool mono = frame.channels == 1;
    if (frame.is_mpeg1) return mono ? 17 : 32;
    return mono ? 9 : 17;
}

}  // namespace

bool parse_id3v2_tags(const std::uint8_t* data, std::size_t size, Mp3Tags& tags,
                      std::size_t& tag_bytes) {
    tag_bytes = 0;
    if (data == nullptr || size < 10) return false;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return false;

    const std::uint8_t major = data[3];
    if (major != 3 && major != 4) return false;  // v2.2 frames are laid out differently
    const std::uint8_t flags = data[5];
    // Unsynchronisation rewrites the tag body to avoid false frame syncs.
    // Undoing it is a second parser; a file that uses it keeps its filename.
    if ((flags & 0x80) != 0) return false;

    const std::size_t payload = synchsafe32(data + 6);
    tag_bytes = 10 + payload;
    if (major == 4 && (flags & 0x10) != 0) tag_bytes += 10;  // footer

    const std::size_t end = std::min(size, 10 + payload);
    std::size_t pos = 10;
    bool found = false;
    while (pos + 10 <= end) {
        const std::uint8_t* frame = data + pos;
        if (frame[0] == 0) break;  // padding starts here

        // v2.4 frame sizes are synchsafe; v2.3 sizes are plain big-endian.
        const std::size_t frame_size =
            major == 4 ? synchsafe32(frame + 4) : big_endian32(frame + 4);
        if (frame_size == 0) break;

        const std::size_t body = pos + 10;
        if (body + frame_size > end) break;  // beyond the bytes we read

        // Format flags cover compression, encryption and grouping, none of which
        // this reader can decode: skip the frame instead of showing its bytes.
        const std::uint8_t format_flags = frame[9];
        if (format_flags == 0) {
            const bool is_title = frame[0] == 'T' && frame[1] == 'I' && frame[2] == 'T' &&
                                  frame[3] == '2';
            const bool is_artist = frame[0] == 'T' && frame[1] == 'P' && frame[2] == 'E' &&
                                   frame[3] == '1';
            const bool is_album = frame[0] == 'T' && frame[1] == 'A' && frame[2] == 'L' &&
                                  frame[3] == 'B';
            if (is_title || is_artist || is_album) {
                std::string text = decode_text_frame(data + body, frame_size);
                if (!text.empty()) {
                    found = true;
                    if (is_title) {
                        tags.title = std::move(text);
                    } else if (is_artist) {
                        tags.artist = std::move(text);
                    } else {
                        tags.album = std::move(text);
                    }
                }
            }
        }
        pos = body + frame_size;
    }
    return found;
}

bool parse_mp3_audio_info(const std::uint8_t* data, std::size_t size, std::size_t start_offset,
                          std::uint64_t file_bytes, Mp3AudioInfo& out) {
    if (data == nullptr || size < 4) return false;

    FrameHeader frame;
    bool found = false;
    std::size_t frame_pos = 0;
    for (std::size_t pos = start_offset; pos + 4 <= size; ++pos) {
        if (data[pos] != 0xFF || (data[pos + 1] & 0xE0) != 0xE0) continue;
        const std::uint32_t header = (static_cast<std::uint32_t>(data[pos]) << 24) |
                                     (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
                                     (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
                                     static_cast<std::uint32_t>(data[pos + 3]);
        if (decode_frame_header(header, frame)) {
            out.sample_rate = frame.sample_rate;
            out.channels = frame.channels;
            out.bitrate_kbps = frame.bitrate_kbps;
            frame_pos = pos;
            found = true;
            break;
        }
    }
    if (!found) return false;

    // A Xing (VBR) or Info (CBR) tag sits right after the frame header's side
    // information.  Its frame count is the only exact duration for a VBR file.
    const std::size_t xing_offset = frame_pos + 4 + side_info_bytes(frame);
    if (xing_offset + 12 <= size) {
        const std::uint8_t* tag = data + xing_offset;
        const bool is_xing = tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g';
        const bool is_info = tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o';
        if (is_xing || is_info) {
            const std::uint32_t flags = (static_cast<std::uint32_t>(tag[4]) << 24) |
                                        (static_cast<std::uint32_t>(tag[5]) << 16) |
                                        (static_cast<std::uint32_t>(tag[6]) << 8) |
                                        static_cast<std::uint32_t>(tag[7]);
            if ((flags & 0x1) != 0) {
                const std::uint32_t frames = (static_cast<std::uint32_t>(tag[8]) << 24) |
                                             (static_cast<std::uint32_t>(tag[9]) << 16) |
                                             (static_cast<std::uint32_t>(tag[10]) << 8) |
                                             static_cast<std::uint32_t>(tag[11]);
                if (frames > 0) {
                    const std::uint64_t samples =
                        static_cast<std::uint64_t>(frames) * frame.samples_per_frame;
                    out.duration_seconds = static_cast<std::uint32_t>(samples / frame.sample_rate);
                    out.from_xing = true;
                }
            }
        }
    }

    if (!out.from_xing) {
        // No frame count: (bytes * 8) / bitrate.  Exact for CBR, an estimate for
        // VBR, and the window we read is far too small to say which it is.
        const std::uint64_t audio_bytes = file_bytes > start_offset ? file_bytes - start_offset : 0;
        const std::uint64_t bitrate_bits = static_cast<std::uint64_t>(frame.bitrate_kbps) * 1000;
        out.duration_seconds =
            bitrate_bits > 0 ? static_cast<std::uint32_t>((audio_bytes * 8) / bitrate_bits) : 0;
    }
    return true;
}

}  // namespace media
