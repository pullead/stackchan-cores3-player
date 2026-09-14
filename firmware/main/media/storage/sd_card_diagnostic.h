#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace media {

inline constexpr std::size_t kSdSectorBytes = 512;
using SdSector = std::array<uint8_t, kSdSectorBytes>;

// Describes already-read sectors. This function never touches the card.
std::string describe_raw_card(const SdSector& mbr, const SdSector& boot_sector);

}  // namespace media
