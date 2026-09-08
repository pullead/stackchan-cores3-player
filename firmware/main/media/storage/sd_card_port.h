#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace media {

inline bool is_supported_wav_filename(std::string_view filename) noexcept {
    if (filename.size() <= 4 || filename.back() == '/' || filename.back() == '\\') {
        return false;
    }

    const std::string_view extension = filename.substr(filename.size() - 4);
    return extension[0] == '.' && (extension[1] == 'w' || extension[1] == 'W') &&
           (extension[2] == 'a' || extension[2] == 'A') && (extension[3] == 'v' || extension[3] == 'V');
}

struct SdTrack {
    std::string path;
    std::string title;
    uint64_t bytes = 0;
};

class SdCardPort {
public:
    SdCardPort() = default;
    ~SdCardPort();

    SdCardPort(const SdCardPort&) = delete;
    SdCardPort& operator=(const SdCardPort&) = delete;

    bool mount();
    void unmount();
    bool is_mounted() const noexcept;
    std::vector<SdTrack> list_tracks();
    const std::string& last_error() const noexcept;

private:
    void* card_ = nullptr;
    std::string last_error_;
};

}  // namespace media
