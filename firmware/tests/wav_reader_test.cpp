#include "media/local/wav_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void append_id(std::vector<uint8_t>& bytes, const char (&id)[5]) {
    bytes.insert(bytes.end(), id, id + 4);
}

void append_chunk(std::vector<uint8_t>& bytes, const char (&id)[5], const std::vector<uint8_t>& payload) {
    append_id(bytes, id);
    append_u32(bytes, static_cast<uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) {
        bytes.push_back(0);
    }
}

std::vector<uint8_t> compatible_wav(uint16_t channels = 1, uint32_t sample_rate = 24000,
                                    uint16_t bits_per_sample = 16, bool data_before_fmt = false) {
    std::vector<uint8_t> bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0);  // Filled after chunks are added.
    append_id(bytes, "WAVE");

    std::vector<uint8_t> fmt;
    append_u16(fmt, 1);
    append_u16(fmt, channels);
    append_u32(fmt, sample_rate);
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits_per_sample / 8));
    append_u32(fmt, sample_rate * block_align);
    append_u16(fmt, block_align);
    append_u16(fmt, bits_per_sample);
    const std::vector<uint8_t> data = {0x34, 0x12, 0xff, 0xff, 0x00, 0x80};
    if (data_before_fmt) {
        append_chunk(bytes, "data", data);
        append_chunk(bytes, "fmt ", fmt);
    } else {
        append_chunk(bytes, "JUNK", {0xa5, 0x5a, 0x01});
        append_chunk(bytes, "fmt ", fmt);
        append_chunk(bytes, "data", data);
    }

    const uint32_t riff_size = static_cast<uint32_t>(bytes.size() - 8);
    bytes[4] = static_cast<uint8_t>(riff_size & 0xff);
    bytes[5] = static_cast<uint8_t>((riff_size >> 8) & 0xff);
    bytes[6] = static_cast<uint8_t>((riff_size >> 16) & 0xff);
    bytes[7] = static_cast<uint8_t>((riff_size >> 24) & 0xff);
    return bytes;
}

bool test_opens_compatible_wav_after_unknown_chunk() {
    const auto bytes = compatible_wav();
    media::WavReader reader;

    return check(reader.open(bytes), "compatible WAV opens") &&
           check(reader.format().sample_rate == 24000, "sample rate is exposed") &&
           check(reader.format().channels == 1, "mono format is exposed") &&
           check(reader.format().bits_per_sample == 16, "bit depth is exposed") &&
           check(reader.data_offset() == 56, "data offset skips padded JUNK and fmt chunks") &&
           check(reader.remaining_frames() == 3, "data size becomes frame count");
}

bool test_reads_frames_and_tracks_remaining_count() {
    const auto bytes = compatible_wav();
    media::WavReader reader;
    int16_t frames[4] = {};
    reader.open(bytes);

    const size_t first_read = reader.read_frames(frames, 2);
    const size_t second_read = reader.read_frames(frames + 2, 2);
    return check(first_read == 2, "first read is limited to requested frames") &&
           check(frames[0] == 0x1234 && frames[1] == -1, "frames are decoded little-endian") &&
           check(reader.remaining_frames() == 0, "all frames consumed") &&
           check(second_read == 1 && frames[2] == -32768,
                 "final partial read returns final frame");
}

bool test_rejects_bad_riff_signature() {
    auto bytes = compatible_wav();
    bytes[0] = 'X';
    media::WavReader reader;
    return check(!reader.open(bytes), "wrong RIFF signature is rejected") &&
           check(reader.remaining_frames() == 0, "failed open resets state");
}

bool test_rejects_bad_wave_signature() {
    auto bytes = compatible_wav();
    bytes[8] = 'X';
    media::WavReader reader;
    return check(!reader.open(bytes), "wrong WAVE signature is rejected");
}

bool test_rejects_chunk_that_runs_past_riff() {
    auto bytes = compatible_wav();
    bytes[16] = 0xff;
    bytes[17] = 0xff;
    bytes[18] = 0xff;
    bytes[19] = 0x7f;
    media::WavReader reader;
    return check(!reader.open(bytes), "out-of-bounds chunk is rejected");
}

bool test_rejects_stereo_44100_and_8bit_formats() {
    const auto stereo = compatible_wav(2, 24000, 16);
    const auto rate = compatible_wav(1, 44100, 16);
    const auto bit_depth = compatible_wav(1, 24000, 8);
    media::WavReader reader;
    return check(!reader.open(stereo), "stereo is rejected") &&
           check(!reader.open(rate), "44100 Hz is rejected") &&
           check(!reader.open(bit_depth), "8-bit PCM is rejected");
}

bool test_rejects_truncated_fmt_chunk() {
    std::vector<uint8_t> bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0);  // Filled after the physically truncated fmt chunk is added.
    append_id(bytes, "WAVE");
    append_id(bytes, "fmt ");
    append_u32(bytes, 16);
    bytes.insert(bytes.end(), 12, 0);
    const uint32_t riff_size = static_cast<uint32_t>(bytes.size() - 8);
    bytes[4] = static_cast<uint8_t>(riff_size & 0xff);
    bytes[5] = static_cast<uint8_t>((riff_size >> 8) & 0xff);
    bytes[6] = static_cast<uint8_t>((riff_size >> 16) & 0xff);
    bytes[7] = static_cast<uint8_t>((riff_size >> 24) & 0xff);
    media::WavReader reader;
    return check(!reader.open(bytes), "physically truncated fmt payload is rejected");
}

bool test_failed_reopen_clears_previous_format_and_data() {
    const auto valid = compatible_wav();
    auto invalid = valid;
    invalid[52] = 0xff;
    invalid[53] = 0xff;
    invalid[54] = 0xff;
    invalid[55] = 0x7f;
    media::WavReader reader;
    reader.open(valid);

    return check(!reader.open(invalid), "invalid reopen is rejected") &&
           check(reader.format().sample_rate == 0, "failed reopen clears format") &&
           check(reader.data_offset() == 0, "failed reopen clears data offset") &&
           check(reader.remaining_frames() == 0, "failed reopen clears frame count");
}

bool test_vector_open_owns_temporary_bytes() {
    media::WavReader reader;
    reader.open(compatible_wav());
    int16_t frame = 0;

    return check(reader.read_frames(&frame, 1) == 1, "temporary WAV remains readable") &&
           check(frame == 0x1234, "temporary WAV first sample is retained");
}

bool test_vector_open_owns_bytes_after_caller_reallocates() {
    auto bytes = compatible_wav();
    media::WavReader reader;
    reader.open(bytes);
    bytes.assign(4096, 0);
    int16_t frame = 0;

    return check(reader.read_frames(&frame, 1) == 1, "reallocated WAV remains readable") &&
           check(frame == 0x1234, "reallocated caller buffer cannot change sample");
}

bool test_rejects_odd_data_size() {
    auto bytes = compatible_wav();
    bytes[52] = 5;
    bytes.pop_back();
    const uint32_t riff_size = static_cast<uint32_t>(bytes.size() - 8);
    bytes[4] = static_cast<uint8_t>(riff_size & 0xff);
    bytes[5] = static_cast<uint8_t>((riff_size >> 8) & 0xff);
    bytes[6] = static_cast<uint8_t>((riff_size >> 16) & 0xff);
    bytes[7] = static_cast<uint8_t>((riff_size >> 24) & 0xff);
    media::WavReader reader;
    return check(!reader.open(bytes), "odd data size is rejected");
}

bool test_rejects_data_before_fmt() {
    const auto bytes = compatible_wav(1, 24000, 16, true);
    media::WavReader reader;
    return check(!reader.open(bytes), "data before fmt is rejected");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_opens_compatible_wav_after_unknown_chunk();
    failures += !test_reads_frames_and_tracks_remaining_count();
    failures += !test_rejects_bad_riff_signature();
    failures += !test_rejects_bad_wave_signature();
    failures += !test_rejects_chunk_that_runs_past_riff();
    failures += !test_rejects_stereo_44100_and_8bit_formats();
    failures += !test_rejects_truncated_fmt_chunk();
    failures += !test_failed_reopen_clears_previous_format_and_data();
    failures += !test_vector_open_owns_temporary_bytes();
    failures += !test_vector_open_owns_bytes_after_caller_reallocates();
    failures += !test_rejects_odd_data_size();
    failures += !test_rejects_data_before_fmt();
    return failures == 0 ? 0 : 1;
}
