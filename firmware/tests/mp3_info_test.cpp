// Host tests for the MP3 metadata reader.
//
// The fixtures are built here rather than embedded as literal arrays so that a
// size or an offset in the test is visibly the same number the parser is
// supposed to find.  Cases that only differ in a byte the parser must interpret
// (v2.3 plain frame sizes against v2.4 synchsafe ones, the Xing side-info
// offset) are called out where they matter.

#include "media/library/mp3_info.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace media;

namespace {

void append_size(std::vector<uint8_t>& out, std::size_t value, bool synchsafe) {
    if (synchsafe) {
        out.push_back(static_cast<uint8_t>((value >> 21) & 0x7F));
        out.push_back(static_cast<uint8_t>((value >> 14) & 0x7F));
        out.push_back(static_cast<uint8_t>((value >> 7) & 0x7F));
        out.push_back(static_cast<uint8_t>(value & 0x7F));
    } else {
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }
}

std::vector<uint8_t> text_bytes(const char* text) {
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

// A frame whose body is already complete, including its encoding byte.
void append_raw_frame(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& body,
                      bool synchsafe_size) {
    out.insert(out.end(), id, id + 4);
    append_size(out, body.size(), synchsafe_size);
    out.push_back(0);
    out.push_back(0);
    out.insert(out.end(), body.begin(), body.end());
}

void append_text_frame(std::vector<uint8_t>& out, const char* id, uint8_t encoding,
                       const std::vector<uint8_t>& text, bool synchsafe_size) {
    std::vector<uint8_t> body;
    body.push_back(encoding);
    body.insert(body.end(), text.begin(), text.end());
    append_raw_frame(out, id, body, synchsafe_size);
}

// The tag size in the header is always synchsafe, in both v2.3 and v2.4.
std::vector<uint8_t> make_tag(uint8_t major, const std::vector<uint8_t>& frames) {
    std::vector<uint8_t> out;
    out.push_back('I');
    out.push_back('D');
    out.push_back('3');
    out.push_back(major);
    out.push_back(0);
    out.push_back(0);
    append_size(out, frames.size(), true);
    out.insert(out.end(), frames.begin(), frames.end());
    return out;
}

std::vector<uint8_t> mpeg1_layer3_stereo_128k_44100() {
    // FF FB 90 00: MPEG-1, Layer III, no CRC, bitrate index 9 (128 kbps),
    // sample-rate index 0 (44100 Hz), no padding, stereo.
    std::vector<uint8_t> frame(64, 0);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x00;
    return frame;
}

}  // namespace

int main() {
    // v2.4, UTF-8 text in title, artist and album.
    {
        std::vector<uint8_t> frames;
        append_text_frame(frames, "TIT2", 3, text_bytes("Hello"), true);
        append_text_frame(frames, "TPE1", 3, text_bytes("ABCD"), true);
        append_text_frame(frames, "TALB", 3, text_bytes("XYZW"), true);
        const auto tag = make_tag(4, frames);

        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(parse_id3v2_tags(tag.data(), tag.size(), tags, tag_bytes));
        assert(tags.title == "Hello");
        assert(tags.artist == "ABCD");
        assert(tags.album == "XYZW");
        assert(tags.any());
        assert(tag_bytes == tag.size());
    }

    // ISO-8859-1 is widened, not copied: 0xE9 must come out as UTF-8 C3 A9.
    {
        std::vector<uint8_t> frames;
        std::vector<uint8_t> latin1;
        latin1.push_back(0xE9);
        append_text_frame(frames, "TPE1", 0, latin1, true);
        const auto tag = make_tag(4, frames);

        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(parse_id3v2_tags(tag.data(), tag.size(), tags, tag_bytes));
        assert(tags.artist == std::string("\xC3\xA9"));
    }

    // v2.3, UTF-16 with a little-endian BOM, and a body long enough that the
    // plain frame size (256) differs from what synchsafe decoding would give
    // (128).  A parser that mixed the two would return half the text.
    {
        std::vector<uint8_t> body;
        body.push_back(0x01);
        body.push_back(0xFF);
        body.push_back(0xFE);
        for (int i = 0; i < 126; ++i) {
            body.push_back('D');
            body.push_back(0x00);
        }
        body.push_back(0x00);  // odd trailing byte: must be ignored, not crash
        assert(body.size() == 256);

        std::vector<uint8_t> frames;
        append_raw_frame(frames, "TIT2", body, false);  // v2.3: plain big-endian size
        const auto tag = make_tag(3, frames);

        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(parse_id3v2_tags(tag.data(), tag.size(), tags, tag_bytes));
        assert(tags.title == std::string(126, 'D'));
        assert(tag_bytes == tag.size());
    }

    // A tag whose unsynchronisation flag is set is refused rather than
    // half-decoded; the caller keeps the filename.
    {
        std::vector<uint8_t> frames;
        append_text_frame(frames, "TIT2", 3, text_bytes("Hidden"), true);
        auto tag = make_tag(4, frames);
        tag[5] = 0x80;

        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(!parse_id3v2_tags(tag.data(), tag.size(), tags, tag_bytes));
        assert(tag_bytes == 0);
    }

    // Nothing to read at all.
    {
        const std::uint8_t junk[32] = {};
        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(!parse_id3v2_tags(junk, sizeof(junk), tags, tag_bytes));
        assert(!parse_id3v2_tags(nullptr, 0, tags, tag_bytes));

        Mp3AudioInfo info;
        assert(!parse_mp3_audio_info(junk, sizeof(junk), 0, 1024, info));
    }

    // MPEG-1 Layer III with a Xing frame count: 10000 frames * 1152 / 44100.
    {
        auto frame = mpeg1_layer3_stereo_128k_44100();
        const std::size_t xing = 4 + 32;  // header + MPEG-1 stereo side information
        frame[xing] = 'X';
        frame[xing + 1] = 'i';
        frame[xing + 2] = 'n';
        frame[xing + 3] = 'g';
        frame[xing + 7] = 0x01;  // flags: frame count present
        const std::uint32_t count = 10000;
        frame[xing + 8] = static_cast<uint8_t>(count >> 24);
        frame[xing + 9] = static_cast<uint8_t>(count >> 16);
        frame[xing + 10] = static_cast<uint8_t>(count >> 8);
        frame[xing + 11] = static_cast<uint8_t>(count);

        Mp3AudioInfo info;
        assert(parse_mp3_audio_info(frame.data(), frame.size(), 0, 0, info));
        assert(info.sample_rate == 44100);
        assert(info.channels == 2);
        assert(info.bitrate_kbps == 128);
        assert(info.from_xing);
        assert(info.duration_seconds == 261);  // 10000 * 1152 / 44100 = 261.2
    }

    // No Xing header: the duration falls back to size and bitrate, which is
    // exact for CBR.  1,000,000 bytes at 128 kbps is 62.5 seconds.
    {
        auto frame = mpeg1_layer3_stereo_128k_44100();
        Mp3AudioInfo info;
        assert(parse_mp3_audio_info(frame.data(), frame.size(), 0, 1000000, info));
        assert(!info.from_xing);
        assert(info.duration_seconds == 62);
    }

    // The first frame is not at offset zero in a real file: the tag is skipped
    // first, and the Xing offset must be measured from the frame, not from the
    // start of the buffer.
    {
        std::vector<uint8_t> frames;
        append_text_frame(frames, "TIT2", 3, text_bytes("Skipped"), true);
        auto tag = make_tag(4, frames);

        auto frame = mpeg1_layer3_stereo_128k_44100();
        const std::size_t xing = 4 + 32;
        frame[xing] = 'X';
        frame[xing + 1] = 'i';
        frame[xing + 2] = 'n';
        frame[xing + 3] = 'g';
        frame[xing + 7] = 0x01;
        frame[xing + 11] = 100;  // 100 * 1152 / 44100 = 2.6 seconds

        std::vector<uint8_t> file = tag;
        file.insert(file.end(), frame.begin(), frame.end());

        Mp3Tags tags;
        std::size_t tag_bytes = 0;
        assert(parse_id3v2_tags(file.data(), file.size(), tags, tag_bytes));
        assert(tags.title == "Skipped");

        Mp3AudioInfo info;
        assert(parse_mp3_audio_info(file.data(), file.size(), tag_bytes, file.size(), info));
        assert(info.from_xing);
        assert(info.duration_seconds == 2);
    }

    return 0;
}
