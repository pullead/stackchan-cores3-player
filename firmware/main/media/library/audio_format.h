#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

// What kind of audio the file actually holds.  The scanner is deliberately more
// permissive than this: it lists names, and this decides what can be decoded.
enum class AudioFormat : std::uint8_t { Unknown, Mp3, Aac, Flac };

// Classifies the bytes at the start of a file.  `offset` reports where the audio
// data begins, which is past an ID3v2 tag when the file carries one.
//
// Only formats this firmware can actually decode are reported.  Ogg (Opus and
// Vorbis) and MP4 (M4A) are containers rather than raw frames; the component's
// Opus and Vorbis decoders want the packets, not the container, so those stay
// Unknown until a demuxer exists -- a file that reports Unknown is refused
// instead of being fed to a decoder that would produce noise.
inline AudioFormat sniff_audio_format(const std::uint8_t* data, std::size_t size,
                                      std::size_t& offset) {
    offset = 0;
    if (data == nullptr || size < 4) {
        return AudioFormat::Unknown;
    }

    // An ID3v2 tag can sit in front of the audio; the size is four synchsafe
    // bytes (seven significant bits each) that exclude the ten-byte header.
    bool tagged = false;
    if (size >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        const std::size_t payload = (static_cast<std::size_t>(data[6] & 0x7F) << 21) |
                                    (static_cast<std::size_t>(data[7] & 0x7F) << 14) |
                                    (static_cast<std::size_t>(data[8] & 0x7F) << 7) |
                                    static_cast<std::size_t>(data[9] & 0x7F);
        offset = 10 + payload;
        tagged = true;
        // 内嵌封面的标签常有几十到上百 KB，远超调用方读取的文件头。ID3v2
        // 几乎只出现在 MP3 上，因此按 MP3 处理，由解码器自行定位第一帧。
        if (offset + 4 > size) {
            return AudioFormat::Mp3;
        }
    }

    const std::uint8_t* at = data + offset;
    if (at[0] == 'f' && at[1] == 'L' && at[2] == 'a' && at[3] == 'C') {
        return AudioFormat::Flac;
    }

    if (at[0] == 0xFF) {
        // ADTS AAC: a twelve-bit sync word followed by a zero layer field, i.e.
        // bits 7..4 all set and bits 2..1 clear.  The ID bit in between is
        // masked out on purpose.
        if ((at[1] & 0xF6) == 0xF0) {
            return AudioFormat::Aac;
        }
        // MPEG audio frame sync: eleven bits set.  Layer III is what the decoder
        // handles; other layers are refused by the frame validation downstream.
        if ((at[1] & 0xE0) == 0xE0) {
            return AudioFormat::Mp3;
        }
    }

    // 标签之后不一定紧跟帧头：不少文件在标签后还有填充或垃圾字节，MP3
    // 解码器会自行重新同步。带标签的文件因此仍按 MP3 处理；不带标签的
    // 文件则保持严格判定，以免把 Ogg/WAV 之类喂给 MP3 解码器。
    if (tagged) {
        return AudioFormat::Mp3;
    }

    return AudioFormat::Unknown;
}

}  // namespace media
