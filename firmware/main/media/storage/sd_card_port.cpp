#include "media/storage/sd_card_port.h"

#include "hal/board/spi3_display_handoff.h"
#include "media/storage/prefetching_stream.h"
#include "media/storage/sd_audio_stream.h"

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

std::unique_ptr<AudioStream> SdCardPort::open_track(const SdTrack& track) {
#ifdef ESP_PLATFORM
    SdAudioFileOperations operations{};
    operations.context = this;
    operations.mount = mount_hardware;
    operations.open = [](void*, std::string_view path, void*& handle, uint64_t& size, std::string& error) {
        FILE* file = fopen(std::string(path).c_str(), "rb");
        if (file == nullptr) { error = "cannot open SD audio file"; return false; }
        if (fseek(file, 0, SEEK_END) != 0) { fclose(file); error = "cannot size SD audio file"; return false; }
        const long end = ftell(file);
        if (end < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); error = "cannot seek SD audio file"; return false; }
        handle = file; size = static_cast<uint64_t>(end); return true;
    };
    operations.read = [](void*, void* handle, uint8_t* destination, std::size_t capacity, bool& io_error, std::string& error) {
        const std::size_t count = fread(destination, 1, capacity, static_cast<FILE*>(handle));
        io_error = ferror(static_cast<FILE*>(handle)) != 0;
        if (io_error) error = "SD audio read failed";
        return count;
    };
    operations.seek = [](void*, void* handle, uint64_t offset, std::string& error) {
        if (offset > static_cast<uint64_t>(LONG_MAX) || fseek(static_cast<FILE*>(handle), static_cast<long>(offset), SEEK_SET) != 0) { error = "SD audio seek failed"; return false; }
        return true;
    };
    operations.close = [](void*, void* handle, std::string& error) { if (fclose(static_cast<FILE*>(handle)) != 0) { error = "SD audio close failed"; return false; } return true; };
    operations.unmount = unmount_hardware;
    auto card_stream = std::make_unique<SdAudioStream>(*handoff_, operations, track.path);
    if (!card_stream->is_open()) {
        return card_stream;  // Let the caller report the open failure.
    }
    // Read ahead on a separate task: decoding must not stop while the display
    // bus is borrowed, which is what held playback below the sample rate.
    return std::make_unique<PrefetchingStream>(std::move(card_stream));
#else
    (void)track;
    return nullptr;
#endif
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

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "media/storage/sd_card_diagnostic.h"
#include "sdmmc_cmd.h"

namespace media {
namespace {

constexpr char kMountPath[] = "/sdcard";
constexpr gpio_num_t kSdChipSelectPin = GPIO_NUM_4;
// Espressif's current CoreS3 IDF BSP uses SPI3_HOST for both the LCD bus and
// the shared-GPIO35 SD bus. Keep the same host and only initialize the bus in
// the board layer; SD is attached after the display handoff is held.
constexpr spi_host_device_t kSdHost = SPI3_HOST;
constexpr size_t kMaxTracks = 64;
constexpr char kTag[] = "SdCardPort";
// Use the probing clock for this isolated diagnostic. GPIO35 is shared with
// LCD D/C, so a slow read separates signal-integrity/edge timing issues from
// filesystem and directory logic without writing to the card.
constexpr int kSdDiagnosticClockKHz = SDMMC_FREQ_PROBING;
// Playback needs real throughput.  The probing clock caps the card at roughly
// 50 KB/s, which cannot keep even a 320 kbps MP3 fed and made playback run at
// about half speed.  Mount fast and fall back to probing if the shared
// GPIO35 bus turns out not to tolerate it.
constexpr int kSdPlaybackClockKHz = SDMMC_FREQ_DEFAULT;

bool ensure_sd_bus(std::string& error) {
    static bool initialized = false;
    if (initialized) {
        return true;
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = GPIO_NUM_37;
    bus_config.miso_io_num = GPIO_NUM_35;
    bus_config.sclk_io_num = GPIO_NUM_36;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 4096;
    const esp_err_t result = spi_bus_initialize(kSdHost, &bus_config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        error = std::string("SD SPI3 bus init failed: ") + esp_err_to_name(result);
        return false;
    }
    initialized = true;
    ESP_LOGI(kTag, "TF SPI3 bus ready on GPIO35/36/37");
    return true;
}

esp_err_t polling_transfer_byte(spi_device_handle_t device, uint8_t transmit, uint8_t& receive) {
    spi_transaction_t transaction = {};
    transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    transaction.length = 8;
    transaction.tx_data[0] = transmit;
    const esp_err_t result = spi_device_polling_transmit(device, &transaction);
    receive = transaction.rx_data[0];
    return result;
}

uint8_t sd_command_crc7(const uint8_t* data, size_t size) {
    uint8_t crc = 0;
    for (size_t byte_index = 0; byte_index < size; ++byte_index) {
        uint8_t byte = data[byte_index];
        for (int bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (((byte ^ crc) & 0x80) != 0) {
                crc ^= 0x09;
            }
            byte <<= 1;
        }
        crc &= 0x7F;
    }
    return static_cast<uint8_t>((crc << 1) | 1);
}

std::string polling_read_sector_after_init(uint32_t sector, SdSector& data) {
    spi_device_interface_config_t device_config = {};
    device_config.clock_speed_hz = kSdDiagnosticClockKHz * 1000;
    device_config.mode = 0;
    device_config.spics_io_num = GPIO_NUM_NC;
    device_config.queue_size = 1;

    spi_device_handle_t device = nullptr;
    esp_err_t result = spi_bus_add_device(kSdHost, &device_config, &device);
    if (result != ESP_OK) {
        return std::string("PIO device init failed: ") + esp_err_to_name(result);
    }

    auto finish = [&device]() {
        gpio_set_level(kSdChipSelectPin, 1);
        uint8_t ignored = 0;
        polling_transfer_byte(device, 0xFF, ignored);
        spi_bus_remove_device(device);
    };

    result = gpio_set_direction(kSdChipSelectPin, GPIO_MODE_OUTPUT);
    if (result == ESP_OK) {
        result = gpio_set_level(kSdChipSelectPin, 1);
    }
    uint8_t received = 0;
    if (result == ESP_OK) {
        result = polling_transfer_byte(device, 0xFF, received);
    }
    if (result == ESP_OK) {
        result = gpio_set_level(kSdChipSelectPin, 0);
    }

    std::array<uint8_t, 6> command = {
        static_cast<uint8_t>(0x40 | 17),
        static_cast<uint8_t>(sector >> 24),
        static_cast<uint8_t>(sector >> 16),
        static_cast<uint8_t>(sector >> 8),
        static_cast<uint8_t>(sector),
        0,
    };
    // The regular SDSPI initialization enables command CRC with CMD59.
    command[5] = sd_command_crc7(command.data(), command.size() - 1);
    for (uint8_t byte : command) {
        if (result != ESP_OK) {
            break;
        }
        result = polling_transfer_byte(device, byte, received);
    }

    uint8_t response = 0xFF;
    for (int attempt = 0; result == ESP_OK && attempt < 16; ++attempt) {
        result = polling_transfer_byte(device, 0xFF, response);
        if ((response & 0x80) == 0) {
            break;
        }
    }
    if (result != ESP_OK) {
        finish();
        return std::string("PIO command transfer failed: ") + esp_err_to_name(result);
    }
    if (response != 0x00) {
        finish();
        char message[48] = {};
        snprintf(message, sizeof(message), "PIO CMD17 response=0x%02X", response);
        return message;
    }

    uint8_t token = 0xFF;
    for (int attempt = 0; attempt < 8192; ++attempt) {
        result = polling_transfer_byte(device, 0xFF, token);
        if (result != ESP_OK || token == 0xFE) {
            break;
        }
        if (token != 0xFF && token != 0x00) {
            break;
        }
    }
    if (result != ESP_OK) {
        finish();
        return std::string("PIO token transfer failed: ") + esp_err_to_name(result);
    }
    if (token != 0xFE) {
        finish();
        char message[48] = {};
        snprintf(message, sizeof(message), "PIO data token=0x%02X", token);
        return message;
    }

    for (uint8_t& byte : data) {
        result = polling_transfer_byte(device, 0xFF, byte);
        if (result != ESP_OK) {
            finish();
            return std::string("PIO data transfer failed: ") + esp_err_to_name(result);
        }
    }
    for (int crc_byte = 0; crc_byte < 2; ++crc_byte) {
        result = polling_transfer_byte(device, 0xFF, received);
        if (result != ESP_OK) {
            finish();
            return std::string("PIO CRC transfer failed: ") + esp_err_to_name(result);
        }
    }

    finish();
    return {};
}

std::string title_from_filename(std::string_view filename) {
    const std::size_t slash = filename.find_last_of("/\\");
    const std::size_t basename_start = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = filename.find_last_of('.');
    const std::size_t title_end = dot == std::string_view::npos || dot < basename_start ? filename.size() : dot;
    return std::string(filename.substr(basename_start, title_end - basename_start));
}

std::string raw_card_diagnostic() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSdHost;
    host.max_freq_khz = kSdDiagnosticClockKHz;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = kSdHost;
    slot_config.gpio_cs = kSdChipSelectPin;
    // GPIO35 is shared with LCD D/C and can retain a display-side level while
    // the card is deselected. Do not make SDSPI wait on that line before the
    // first command; the card protocol itself determines command readiness.
    // Match the CoreS3 BSP/default SDSPI configuration now that GPIO35 is
    // explicitly released and routed to SPI3_Q before device initialization.
    slot_config.wait_for_miso = 0;

    sdspi_dev_handle_t device = -1;
    esp_err_t result = sdspi_host_init_device(&slot_config, &device);
    if (result != ESP_OK) {
        return std::string("raw device init failed: ") + esp_err_to_name(result);
    }

    host.slot = device;
    sdmmc_card_t card = {};
    alignas(4) SdSector mbr = {};
    alignas(4) SdSector boot_sector = {};
    std::string description;

    result = sdmmc_card_init(&host, &card);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "TF raw init ok: ocr=0x%08" PRIx32 " rca=%u mem=%u sdio=%u csd_capacity=%d sectorsize=%d max=%" PRIu32 "kHz real=%dkHz",
                 card.ocr, card.rca, card.is_mem, card.is_sdio, card.csd.capacity,
                 card.csd.sector_size, card.max_freq_khz, card.real_freq_khz);
        ESP_LOGI(kTag, "TF card identity: mfg=%u oem=0x%04X name=%s revision=%u.%u serial=0x%08" PRIx32,
                 card.cid.mfg_id, card.cid.oem_id, card.cid.name, card.cid.revision >> 4,
                 card.cid.revision & 0x0F, card.cid.serial);
    } else {
        ESP_LOGW(kTag, "TF raw init failed: %s", esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = sdmmc_read_sectors(&card, mbr.data(), 0, 1);
        ESP_LOGI(kTag, "TF raw sector 0 result=%s first16=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 esp_err_to_name(result), mbr[0], mbr[1], mbr[2], mbr[3], mbr[4], mbr[5], mbr[6], mbr[7],
                 mbr[8], mbr[9], mbr[10], mbr[11], mbr[12], mbr[13], mbr[14], mbr[15]);
        SdSector alternate = {};
        const esp_err_t alternate_result = sdmmc_read_sectors(&card, alternate.data(), 2048, 1);
        ESP_LOGI(kTag, "TF raw sector 2048 result=%s first16=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 esp_err_to_name(alternate_result), alternate[0], alternate[1], alternate[2], alternate[3],
                 alternate[4], alternate[5], alternate[6], alternate[7], alternate[8], alternate[9],
                 alternate[10], alternate[11], alternate[12], alternate[13], alternate[14], alternate[15]);
    }

    // Keep this read-only fallback separate from the normal SDSPI data path.
    // Byte-sized inline transactions avoid DMA and tell us whether the card's
    // data is reaching GPIO35 even when a 512-byte SDSPI transfer reads zeros.
    sdspi_host_remove_device(device);
    device = -1;
    const std::array<uint32_t, 7> sample_sectors = {0, 1, 63, 2048, 8192, 32768, 65536};
    for (uint32_t sample_sector : sample_sectors) {
        alignas(4) SdSector polling_sector = {};
        const std::string polling_error = polling_read_sector_after_init(sample_sector, polling_sector);
        if (polling_error.empty()) {
            const size_t nonzero_bytes = static_cast<size_t>(std::count_if(
                polling_sector.begin(), polling_sector.end(), [](uint8_t byte) { return byte != 0; }));
            ESP_LOGW(kTag, "TF PIO sector %" PRIu32 " nonzero=%u first16=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X signature=%02X%02X",
                     sample_sector, static_cast<unsigned>(nonzero_bytes),
                     polling_sector[0], polling_sector[1], polling_sector[2], polling_sector[3],
                     polling_sector[4], polling_sector[5], polling_sector[6], polling_sector[7],
                     polling_sector[8], polling_sector[9], polling_sector[10], polling_sector[11],
                     polling_sector[12], polling_sector[13], polling_sector[14], polling_sector[15],
                     polling_sector[510], polling_sector[511]);
        } else {
            ESP_LOGW(kTag, "TF PIO sector %" PRIu32 " failed: %s", sample_sector, polling_error.c_str());
        }
    }
    if (result != ESP_OK) {
        description = std::string("raw sector read failed: ") + esp_err_to_name(result);
    } else if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        description = describe_raw_card(mbr, boot_sector);
    } else {
        const uint32_t partition_lba = static_cast<uint32_t>(mbr[446 + 8]) |
                                       (static_cast<uint32_t>(mbr[446 + 9]) << 8) |
                                       (static_cast<uint32_t>(mbr[446 + 10]) << 16) |
                                       (static_cast<uint32_t>(mbr[446 + 11]) << 24);
        result = sdmmc_read_sectors(&card, boot_sector.data(), partition_lba, 1);
        if (result != ESP_OK) {
            description = std::string("raw boot sector read failed: ") + esp_err_to_name(result);
        } else {
            description = describe_raw_card(mbr, boot_sector);
        }
    }

    if (device >= 0) {
        sdspi_host_remove_device(device);
    }
    return description;
}

}  // namespace

bool SdCardPort::mount_hardware(void* raw_context, std::string& error) {
    auto& port = *static_cast<SdCardPort*>(raw_context);
    if (port.card_ != nullptr) {
        error = "SD card is already mounted";
        return false;
    }

    if (!ensure_sd_bus(error)) {
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = kSdHost;
    slot_config.gpio_cs = kSdChipSelectPin;
    slot_config.wait_for_miso = 0;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;

    sdmmc_card_t* card = nullptr;
    esp_err_t result = ESP_FAIL;
    for (const int clock_khz : {kSdPlaybackClockKHz, kSdDiagnosticClockKHz}) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = kSdHost;
        host.max_freq_khz = clock_khz;

        ESP_LOGI(kTag, "TF browse-only mount at %d kHz", clock_khz);
        result = esp_vfs_fat_sdspi_mount(kMountPath, &host, &slot_config, &mount_config, &card);
        if (result == ESP_OK) {
            ESP_LOGI(kTag, "TF mounted at %d kHz, real clock %d kHz", clock_khz,
                     card != nullptr ? card->real_freq_khz : 0);
            break;
        }
        ESP_LOGW(kTag, "TF mount at %d kHz failed: %s", clock_khz, esp_err_to_name(result));
    }

    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        const std::string raw_description = raw_card_diagnostic();
        if (!raw_description.empty()) {
            error += "; " + raw_description;
            ESP_LOGW(kTag, "TF raw read-only diagnostic: %s", raw_description.c_str());
        }
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
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        ESP_LOGW(kTag, "TF unmount failed: %s", error.c_str());
        return false;
    }
    port.card_ = nullptr;
    return true;
}

bool SdCardPort::list_hardware_tracks(void* raw_context, std::vector<SdTrack>& tracks, std::string& error) {
    auto& port = *static_cast<SdCardPort*>(raw_context);
    if (port.card_ == nullptr) {
        error = "SD card is not mounted";
        return false;
    }

    bool success = true;
    std::vector<std::pair<std::string, unsigned>> directories;
    directories.emplace_back(kMountPath, 0U);

    while (!directories.empty() && tracks.size() < kMaxTracks) {
        const auto [directory_path, depth] = std::move(directories.back());
        directories.pop_back();
        DIR* directory = opendir(directory_path.c_str());
        if (directory == nullptr) {
            if (directory_path == kMountPath) {
                error = std::string("cannot open SD card root: ") + std::strerror(errno);
                success = false;
            }
            continue;
        }

        while (tracks.size() < kMaxTracks) {
            errno = 0;
            dirent* entry = readdir(directory);
            if (entry == nullptr) {
                if (errno != 0) {
                    error = std::string("cannot read SD card directory: ") + std::strerror(errno);
                    success = false;
                }
                break;
            }
            const std::string_view filename(entry->d_name);
            if (filename == "." || filename == "..") {
                continue;
            }

            const std::string path = directory_path + "/" + std::string(filename);
            struct stat status = {};
            if (stat(path.c_str(), &status) != 0) {
                continue;
            }
            if (S_ISDIR(status.st_mode)) {
                if (depth < 2U) {
                    directories.emplace_back(path, depth + 1U);
                }
                continue;
            }
            if (!S_ISREG(status.st_mode) || !is_supported_audio_filename(filename)) {
                continue;
            }
            tracks.push_back({path, title_from_filename(filename), static_cast<uint64_t>(status.st_size)});
        }
        if (closedir(directory) != 0) {
            const std::string close_error = std::string("cannot close SD card directory: ") + std::strerror(errno);
            if (!error.empty()) {
                error += "; ";
            }
            error += close_error;
            success = false;
        }
    }
    return success;
}

}  // namespace media

#endif
