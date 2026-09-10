#include "media/storage/sd_card_diagnostic.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

void set_le32(std::array<uint8_t, 512>& sector, std::size_t offset, uint32_t value) {
    sector[offset] = static_cast<uint8_t>(value);
    sector[offset + 1] = static_cast<uint8_t>(value >> 8);
    sector[offset + 2] = static_cast<uint8_t>(value >> 16);
    sector[offset + 3] = static_cast<uint8_t>(value >> 24);
}

bool test_describes_fat32_partition() {
    std::array<uint8_t, 512> mbr{};
    std::array<uint8_t, 512> boot{};
    mbr[446 + 4] = 0x0C;
    set_le32(mbr, 446 + 8, 2048);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    std::memcpy(boot.data() + 82, "FAT32   ", 8);
    const auto info = media::describe_raw_card(mbr, boot);
    return check(info == "MBR p1=0x0c start=2048 FAT32", "FAT32 partition is described");
}

bool test_describes_exfat_partition() {
    std::array<uint8_t, 512> mbr{};
    std::array<uint8_t, 512> boot{};
    mbr[446 + 4] = 0x07;
    set_le32(mbr, 446 + 8, 4096);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    std::memcpy(boot.data() + 3, "EXFAT   ", 8);
    const auto info = media::describe_raw_card(mbr, boot);
    return check(info == "MBR p1=0x07 start=4096 EXFAT", "exFAT partition is described");
}

bool test_rejects_invalid_mbr_signature() {
    std::array<uint8_t, 512> mbr{};
    std::array<uint8_t, 512> boot{};
    const auto info = media::describe_raw_card(mbr, boot);
    return check(info == "MBR SIGNATURE INVALID", "invalid MBR is reported");
}

}  // namespace

int main() {
    int failures = 0;
    failures += !test_describes_fat32_partition();
    failures += !test_describes_exfat_partition();
    failures += !test_rejects_invalid_mbr_signature();
    return failures == 0 ? 0 : 1;
}
