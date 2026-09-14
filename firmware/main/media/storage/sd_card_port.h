#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
};

class SdCardPort {
public:
    SdCardPort();
    SdCardPort(board::Spi3DisplayHandoff& handoff, SdCardOperations operations) noexcept;
    ~SdCardPort() = default;

    SdCardPort(const SdCardPort&) = delete;
    SdCardPort& operator=(const SdCardPort&) = delete;

    std::vector<SdTrack> browse_tracks();
    const std::string& last_error() const noexcept;

private:
    bool operations_ready() const noexcept;
    void append_error(std::string_view error);

#ifdef ESP_PLATFORM
    static bool mount_hardware(void* context, std::string& error);
    static bool list_hardware_tracks(void* context, std::vector<SdTrack>& tracks, std::string& error);
    static bool unmount_hardware(void* context, std::string& error);
#endif

    board::Spi3DisplayHandoff* handoff_ = nullptr;
    SdCardOperations operations_;
    void* card_ = nullptr;
    std::string last_error_;
};

}  // namespace media
