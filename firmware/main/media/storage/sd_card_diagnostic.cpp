#include "media/storage/sd_card_diagnostic.h"

#include <iomanip>
#include <sstream>

namespace media {
namespace {

uint32_t read_le32(const SdSector& sector, std::size_t offset) {
    return static_cast<uint32_t>(sector[offset]) |
           (static_cast<uint32_t>(sector[offset + 1]) << 8) |
           (static_cast<uint32_t>(sector[offset + 2]) << 16) |
           (static_cast<uint32_t>(sector[offset + 3]) << 24);
}

bool has_ascii(const SdSector& sector, std::size_t offset, const char* text, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        if (sector[offset + index] != static_cast<uint8_t>(text[index])) {
            return false;
        }
    }
    return true;
}

const char* filesystem_name(const SdSector& boot_sector, uint8_t partition_type) {
    if (has_ascii(boot_sector, 3, "EXFAT   ", 8)) {
        return "EXFAT";
    }
    if (has_ascii(boot_sector, 82, "FAT32   ", 8) || partition_type == 0x0B || partition_type == 0x0C) {
        return "FAT32";
    }
    if (partition_type == 0x07) {
        return "NTFS/EXFAT?";
    }
    if (partition_type == 0xEE) {
        return "GPT PROTECTIVE";
    }
    return "UNKNOWN";
}

}  // namespace

std::string describe_raw_card(const SdSector& mbr, const SdSector& boot_sector) {
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return "MBR SIGNATURE INVALID";
    }

    constexpr std::size_t kFirstPartition = 446;
    const uint8_t partition_type = mbr[kFirstPartition + 4];
    const uint32_t partition_lba = read_le32(mbr, kFirstPartition + 8);

    std::ostringstream output;
    output << "MBR p1=0x" << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned int>(partition_type) << std::dec << " start=" << partition_lba << ' '
           << filesystem_name(boot_sector, partition_type);
    return output.str();
}

}  // namespace media
