// Host tests for the container sniffer that decides which decoder a file gets.

#include "media/library/audio_format.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace media;

namespace {

std::vector<std::uint8_t> bytes(const char* text) {
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

// An ID3v2 header whose payload is `payload` bytes of padding.
std::vector<std::uint8_t> id3_header(std::size_t payload) {
    std::vector<std::uint8_t> out = {'I', 'D', '3', 4, 0, 0};
    out.push_back(static_cast<std::uint8_t>((payload >> 21) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((payload >> 14) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((payload >> 7) & 0x7F));
    out.push_back(static_cast<std::uint8_t>(payload & 0x7F));
    out.insert(out.end(), payload, 0);
    return out;
}

}  // namespace

int main() {
    // MP3: an MPEG frame sync, with and without an ID3v2 tag in front.  0xFB is
    // MPEG-1 Layer III, which must not be mistaken for ADTS AAC.
    {
        std::size_t offset = 0;
        const std::uint8_t frame[] = {0xFF, 0xFB, 0x90, 0x00};
        assert(sniff_audio_format(frame, sizeof(frame), offset) == AudioFormat::Mp3);
        assert(offset == 0);

        auto tagged = id3_header(20);
        tagged.insert(tagged.end(), std::begin(frame), std::end(frame));
        offset = 0;
        assert(sniff_audio_format(tagged.data(), tagged.size(), offset) == AudioFormat::Mp3);
        assert(offset == 30);
    }

    // ADTS AAC shares the 0xFF first byte but has a zero layer field.
    {
        std::size_t offset = 0;
        const std::uint8_t adts[] = {0xFF, 0xF1, 0x50, 0x80};
        assert(sniff_audio_format(adts, sizeof(adts), offset) == AudioFormat::Aac);
        const std::uint8_t adts_other_id[] = {0xFF, 0xF9, 0x4C, 0x80};
        assert(sniff_audio_format(adts_other_id, sizeof(adts_other_id), offset) == AudioFormat::Aac);
    }

    // FLAC announces itself; a tag in front is allowed but unusual.
    {
        std::size_t offset = 0;
        auto flac = bytes("fLaC");
        flac.push_back(0x00);
        assert(sniff_audio_format(flac.data(), flac.size(), offset) == AudioFormat::Flac);
    }

    // Containers this firmware cannot demux are refused rather than guessed at:
    // the component's Opus and Vorbis decoders take packets, not Ogg pages.
    {
        std::size_t offset = 0;
        auto ogg = bytes("OggS");
        ogg.push_back(0x00);
        assert(sniff_audio_format(ogg.data(), ogg.size(), offset) == AudioFormat::Unknown);

        auto m4a = std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p'};
        assert(sniff_audio_format(m4a.data(), m4a.size(), offset) == AudioFormat::Unknown);

        auto wav = bytes("RIFF");
        wav.push_back(0x00);
        assert(sniff_audio_format(wav.data(), wav.size(), offset) == AudioFormat::Unknown);
    }

    // Short and empty inputs are refused, not read past the end.
    {
        std::size_t offset = 0;
        const std::uint8_t two[] = {0xFF, 0xFB};
        assert(sniff_audio_format(two, sizeof(two), offset) == AudioFormat::Unknown);
        assert(sniff_audio_format(nullptr, 0, offset) == AudioFormat::Unknown);
        assert(offset == 0);

        // A tag that claims more payload than was read cannot be classified.
        auto truncated = id3_header(0);
        truncated.resize(6);
        assert(sniff_audio_format(truncated.data(), truncated.size(), offset) == AudioFormat::Unknown);
    }

    return 0;
}
