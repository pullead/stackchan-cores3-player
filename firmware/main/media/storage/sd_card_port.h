#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include "media/decoder/audio_stream.h"
#include "media/storage/sd_audio_stream.h"

namespace board {
class Spi3DisplayHandoff;
}

namespace media {

inline bool is_supported_audio_filename(std::string_view filename) noexcept {
    if (filename.size() <= 4 || filename.back() == '/' || filename.back() == '\\') {
        return false;
    }

    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= filename.size()) {
        return false;
    }

    std::string extension(filename.substr(dot + 1));
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension == "wav" || extension == "mp3" || extension == "aac" || extension == "m4a" ||
           extension == "flac" || extension == "ogg" || extension == "opus";
}

inline bool is_supported_wav_filename(std::string_view filename) noexcept {
    if (!is_supported_audio_filename(filename)) {
        return false;
    }
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || filename.size() - dot != 4) {
        return false;
    }
    const auto lower = [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    };
    return lower(filename[dot + 1]) == 'w' && lower(filename[dot + 2]) == 'a' && lower(filename[dot + 3]) == 'v';
}

struct SdTrack {
    std::string path;
    std::string title;
    uint64_t bytes = 0;
};

struct SdCardOperations {
    void* context = nullptr;
    bool (*mount)(void* context, std::string& error) = nullptr;
    bool (*list_tracks)(void* context, std::vector<SdTrack>& tracks, std::string& error) = nullptr;
    bool (*unmount)(void* context, std::string& error) = nullptr;
    // Optional.  Reads the first bytes of a file for the metadata indexer, which
    // must not pay for a prefetch ring and a task per file.
    bool (*read_head)(void* context, std::string_view path, std::vector<uint8_t>& buffer,
                      std::size_t max_bytes, std::string& error) = nullptr;
};

class SdCardPort {
public:
    SdCardPort();
    SdCardPort(board::Spi3DisplayHandoff& handoff, SdCardOperations operations) noexcept;
    ~SdCardPort() = default;

    SdCardPort(const SdCardPort&) = delete;
    SdCardPort& operator=(const SdCardPort&) = delete;

    std::vector<SdTrack> browse_tracks();
    std::unique_ptr<AudioStream> open_track(const SdTrack& track);
    // Reads up to `max_bytes` from the start of a track through the same bus
    // handoff the player uses, but without the playback stream.
    bool read_head(const SdTrack& track, std::vector<uint8_t>& buffer, std::size_t max_bytes);
    const std::string& last_error() const noexcept;

private:
    bool operations_ready() const noexcept;
    void append_error(std::string_view error);

#ifdef ESP_PLATFORM
    static bool mount_hardware(void* context, std::string& error);
    static bool list_hardware_tracks(void* context, std::vector<SdTrack>& tracks, std::string& error);
    static bool unmount_hardware(void* context, std::string& error);
    static bool read_head_hardware(void* context, std::string_view path, std::vector<uint8_t>& buffer,
                                   std::size_t max_bytes, std::string& error);
    // The file callbacks SdAudioStream needs to read one card file.  Both the
    // player and the metadata indexer go through this, so they cannot drift.
    static SdAudioFileOperations make_file_operations(void* context);
#endif

    board::Spi3DisplayHandoff* handoff_ = nullptr;
    SdCardOperations operations_;
    void* card_ = nullptr;
    std::string last_error_;
};

}  // namespace media
