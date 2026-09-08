#include "media/local/wav_reader.h"

#include <limits>

namespace media {
namespace {

constexpr size_t kRiffHeaderSize = 12;
constexpr size_t kChunkHeaderSize = 8;
constexpr size_t kPcmFormatSize = 16;
constexpr size_t kBytesPerFrame = 2;

bool has_id(const uint8_t* bytes, size_t offset, const char (&id)[5]) noexcept {
    return bytes[offset] == static_cast<uint8_t>(id[0]) && bytes[offset + 1] == static_cast<uint8_t>(id[1]) &&
           bytes[offset + 2] == static_cast<uint8_t>(id[2]) && bytes[offset + 3] == static_cast<uint8_t>(id[3]);
}

uint16_t read_u16(const uint8_t* bytes, size_t offset) noexcept {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t read_u32(const uint8_t* bytes, size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool add_within_limit(size_t value, size_t amount, size_t limit, size_t* result) noexcept {
    if (value > limit || amount > limit - value) {
        return false;
    }
    *result = value + amount;
    return true;
}

}  // namespace

bool WavReader::open(const std::vector<uint8_t>& bytes) noexcept {
    return open(bytes.data(), bytes.size());
}

bool WavReader::open(const uint8_t* bytes, size_t size) noexcept {
    reset();
    const auto fail = [this]() noexcept {
        reset();
        return false;
    };
    if (bytes == nullptr || size < kRiffHeaderSize || !has_id(bytes, 0, "RIFF") || !has_id(bytes, 8, "WAVE")) {
        return fail();
    }

    const uint32_t declared_riff_size = read_u32(bytes, 4);
    size_t riff_end = 0;
    if (!add_within_limit(8, static_cast<size_t>(declared_riff_size), size, &riff_end) || riff_end < kRiffHeaderSize) {
        return fail();
    }

    bool have_format = false;
    bool have_data = false;
    size_t cursor = kRiffHeaderSize;
    while (cursor < riff_end) {
        size_t payload_offset = 0;
        if (!add_within_limit(cursor, kChunkHeaderSize, riff_end, &payload_offset)) {
            return fail();
        }

        const uint32_t chunk_size_u32 = read_u32(bytes, cursor + 4);
        const size_t chunk_size = static_cast<size_t>(chunk_size_u32);
        size_t payload_end = 0;
        if (!add_within_limit(payload_offset, chunk_size, riff_end, &payload_end)) {
            return fail();
        }

        if (has_id(bytes, cursor, "fmt ")) {
            if (have_format || chunk_size < kPcmFormatSize) {
                return fail();
            }
            const uint16_t audio_format = read_u16(bytes, payload_offset);
            const uint16_t channels = read_u16(bytes, payload_offset + 2);
            const uint32_t sample_rate = read_u32(bytes, payload_offset + 4);
            const uint32_t byte_rate = read_u32(bytes, payload_offset + 8);
            const uint16_t block_align = read_u16(bytes, payload_offset + 12);
            const uint16_t bits_per_sample = read_u16(bytes, payload_offset + 14);
            if (audio_format != 1 || channels != 1 || sample_rate != 24000 || bits_per_sample != 16 ||
                block_align != kBytesPerFrame || byte_rate != sample_rate * block_align) {
                return fail();
            }
            format_ = {sample_rate, static_cast<uint8_t>(channels), static_cast<uint8_t>(bits_per_sample)};
            have_format = true;
        } else if (has_id(bytes, cursor, "data")) {
            if (have_data || (chunk_size & 1U) != 0U) {
                return fail();
            }
            data_offset_ = payload_offset;
            data_end_ = payload_end;
            position_ = payload_offset;
            have_data = true;
        }

        const size_t padded_chunk_size = chunk_size + (chunk_size & 1U);
        size_t next_cursor = 0;
        if (padded_chunk_size < chunk_size || !add_within_limit(payload_offset, padded_chunk_size, riff_end, &next_cursor)) {
            return fail();
        }
        cursor = next_cursor;
    }

    if (!have_format || !have_data) {
        return fail();
    }
    bytes_ = bytes;
    open_ = true;
    return true;
}

const PcmFormat& WavReader::format() const noexcept {
    return format_;
}

size_t WavReader::data_offset() const noexcept {
    return data_offset_;
}

size_t WavReader::remaining_frames() const noexcept {
    return open_ ? (data_end_ - position_) / kBytesPerFrame : 0;
}

size_t WavReader::read_frames(int16_t* destination, size_t max_frames) noexcept {
    if (!open_ || destination == nullptr || max_frames == 0) {
        return 0;
    }
    const size_t frames = remaining_frames() < max_frames ? remaining_frames() : max_frames;
    for (size_t index = 0; index < frames; ++index) {
        destination[index] = static_cast<int16_t>(read_u16(bytes_, position_));
        position_ += kBytesPerFrame;
    }
    return frames;
}

void WavReader::reset() noexcept {
    bytes_ = nullptr;
    data_offset_ = 0;
    data_end_ = 0;
    position_ = 0;
    format_ = {};
    open_ = false;
}

}  // namespace media
