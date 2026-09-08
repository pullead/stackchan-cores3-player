#include "media/storage/sd_card_port.h"

#ifdef ESP_PLATFORM

#include <dirent.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace media {
namespace {

constexpr char kMountPath[] = "/sdcard";
constexpr gpio_num_t kSdChipSelectPin = GPIO_NUM_4;
constexpr size_t kMaxTracks = 64;
constexpr char kTag[] = "SdCardPort";

std::string title_from_filename(std::string_view filename) {
    return std::string(filename.substr(0, filename.size() - 4));
}

}  // namespace

SdCardPort::~SdCardPort() {
    unmount();
}

bool SdCardPort::mount() {
    unmount();
    last_error_.clear();

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI3_HOST;
    slot_config.gpio_cs = kSdChipSelectPin;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;

    sdmmc_card_t* card = nullptr;
    const esp_err_t result = esp_vfs_fat_sdspi_mount(kMountPath, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        last_error_ = esp_err_to_name(result);
        ESP_LOGW(kTag, "TF browse-only mount failed: %s", last_error_.c_str());
        return false;
    }

    card_ = card;
    ESP_LOGI(kTag, "TF browse-only mount ready; sharing SPI3 with the display");
    return true;
}

void SdCardPort::unmount() {
    if (card_ == nullptr) {
        return;
    }

    const esp_err_t result = esp_vfs_fat_sdcard_unmount(kMountPath, static_cast<sdmmc_card_t*>(card_));
    if (result != ESP_OK) {
        last_error_ = esp_err_to_name(result);
        ESP_LOGW(kTag, "TF unmount failed: %s", last_error_.c_str());
    }
    card_ = nullptr;
}

bool SdCardPort::is_mounted() const noexcept {
    return card_ != nullptr;
}

std::vector<SdTrack> SdCardPort::list_tracks() {
    std::vector<SdTrack> tracks;
    if (!is_mounted()) {
        last_error_ = "SD card is not mounted";
        return tracks;
    }

    DIR* directory = opendir(kMountPath);
    if (directory == nullptr) {
        last_error_ = "cannot open SD card root";
        return tracks;
    }

    while (tracks.size() < kMaxTracks) {
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            break;
        }
        const std::string_view filename(entry->d_name);
        if (!is_supported_wav_filename(filename)) {
            continue;
        }

        const std::string path = std::string(kMountPath) + "/" + std::string(filename);
        struct stat status = {};
        if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
            continue;
        }
        tracks.push_back({path, title_from_filename(filename), static_cast<uint64_t>(status.st_size)});
    }
    closedir(directory);
    last_error_.clear();
    return tracks;
}

const std::string& SdCardPort::last_error() const noexcept {
    return last_error_;
}

}  // namespace media

#else

namespace media {

SdCardPort::~SdCardPort() = default;

bool SdCardPort::mount() {
    last_error_ = "SD card is only available on ESP hardware";
    return false;
}

void SdCardPort::unmount() {}

bool SdCardPort::is_mounted() const noexcept {
    return false;
}

std::vector<SdTrack> SdCardPort::list_tracks() {
    last_error_ = "SD card is only available on ESP hardware";
    return {};
}

const std::string& SdCardPort::last_error() const noexcept {
    return last_error_;
}

}  // namespace media

#endif
