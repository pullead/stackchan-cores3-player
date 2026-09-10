#include "media/storage/sd_card_port.h"

#include "hal/board/spi3_display_handoff.h"

namespace media {

SdCardPort::SdCardPort()
#ifdef ESP_PLATFORM
    : handoff_(&board::get_spi3_display_handoff()),
      operations_{this, mount_hardware, list_hardware_tracks, unmount_hardware}
#endif
{}

SdCardPort::SdCardPort(board::Spi3DisplayHandoff& handoff, SdCardOperations operations) noexcept
    : handoff_(&handoff), operations_(operations) {}

std::vector<SdTrack> SdCardPort::browse_tracks() {
    std::vector<SdTrack> tracks;
    last_error_.clear();

#ifndef ESP_PLATFORM
    if (handoff_ == nullptr) {
        last_error_ = "SD card is only available on ESP hardware";
        return tracks;
    }
#endif

    if (handoff_ == nullptr || !operations_ready()) {
        last_error_ = "SD card browse port is not initialized";
        return tracks;
    }

    board::Spi3DisplayHandoffGuard handoff_guard(*handoff_);
    if (!handoff_guard.acquired()) {
        const std::string handoff_error = handoff_->last_error();
        last_error_ = handoff_error;
        return tracks;
    }

    std::string operation_error;
    if (!operations_.mount(operations_.context, operation_error)) {
        append_error(operation_error.empty() ? "SD mount failed" : operation_error);
        if (!handoff_guard.release()) {
            const std::string release_error = handoff_->last_error();
            append_error(release_error);
        }
        return tracks;
    }

    operation_error.clear();
    const bool listed = operations_.list_tracks(operations_.context, tracks, operation_error);
    if (!listed) {
        append_error(operation_error.empty() ? "SD directory read failed" : operation_error);
    }

    operation_error.clear();
    const bool unmounted = operations_.unmount(operations_.context, operation_error);
    if (!unmounted) {
        append_error(operation_error.empty() ? "SD unmount failed" : operation_error);
    }

    const bool released = handoff_guard.release();
    if (!released) {
        const std::string release_error = handoff_->last_error();
        append_error(release_error);
    }

    if (!listed || !unmounted || !released) {
        tracks.clear();
    }
    return tracks;
}

const std::string& SdCardPort::last_error() const noexcept {
    return last_error_;
}

bool SdCardPort::operations_ready() const noexcept {
    return operations_.mount != nullptr && operations_.list_tracks != nullptr && operations_.unmount != nullptr;
}

void SdCardPort::append_error(std::string_view error) {
    if (error.empty()) {
        return;
    }
    if (!last_error_.empty()) {
        last_error_ += "; ";
    }
    last_error_.append(error.data(), error.size());
}

}  // namespace media

#ifdef ESP_PLATFORM

#include <cerrno>
#include <cstring>
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

bool SdCardPort::mount_hardware(void* raw_context, std::string& error) {
    auto& port = *static_cast<SdCardPort*>(raw_context);
    if (port.card_ != nullptr) {
        error = "SD card is already mounted";
        return false;
    }

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
        error = esp_err_to_name(result);
        ESP_LOGW(kTag, "TF browse-only mount failed: %s", error.c_str());
        return false;
    }

    port.card_ = card;
    ESP_LOGI(kTag, "TF browse-only mount ready inside display handoff");
    return true;
}

bool SdCardPort::unmount_hardware(void* raw_context, std::string& error) {
    auto& port = *static_cast<SdCardPort*>(raw_context);
    if (port.card_ == nullptr) {
        error = "SD card is not mounted";
        return false;
    }

    const esp_err_t result = esp_vfs_fat_sdcard_unmount(kMountPath, static_cast<sdmmc_card_t*>(port.card_));
    port.card_ = nullptr;
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        ESP_LOGW(kTag, "TF unmount failed: %s", error.c_str());
        return false;
    }
    return true;
}

bool SdCardPort::list_hardware_tracks(void* raw_context, std::vector<SdTrack>& tracks, std::string& error) {
    auto& port = *static_cast<SdCardPort*>(raw_context);
    if (port.card_ == nullptr) {
        error = "SD card is not mounted";
        return false;
    }

    DIR* directory = opendir(kMountPath);
    if (directory == nullptr) {
        error = std::string("cannot open SD card root: ") + std::strerror(errno);
        return false;
    }

    bool success = true;
    while (tracks.size() < kMaxTracks) {
        errno = 0;
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                error = std::string("cannot read SD card root: ") + std::strerror(errno);
                success = false;
            }
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

    if (closedir(directory) != 0) {
        const std::string close_error = std::string("cannot close SD card root: ") + std::strerror(errno);
        if (!error.empty()) {
            error += "; ";
        }
        error += close_error;
        success = false;
    }
    return success;
}

}  // namespace media

#endif
