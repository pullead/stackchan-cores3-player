#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace media {

// Stable identity for a track: FNV-1a 32 over the path, with ASCII letters
// lowercased first.  The lowercasing is not cosmetic: a card that lists both
// /Music and /music would otherwise index the same file twice, and the same
// path must keep its identity across reboots because this is what favourites
// are stored against.
std::uint32_t make_track_id(std::string_view path) noexcept;

// Where the favourite set lives.  The firmware passes the NVS port; host tests
// pass memory.  Nothing here ever writes to the SD card, which is shared with
// another player and must stay untouched.
struct LibraryStorePort {
    void* context = nullptr;
    // Writes up to `capacity` ids into `ids` and returns how many it read.
    std::size_t (*load)(void* context, std::uint32_t* ids, std::size_t capacity) = nullptr;
    bool (*save)(void* context, const std::uint32_t* ids, std::size_t count) = nullptr;
};

class LibraryStore {
public:
    LibraryStore() = default;
    explicit LibraryStore(LibraryStorePort port) noexcept : port_(port) {}

    bool ready() const noexcept { return port_.load != nullptr && port_.save != nullptr; }

    // Reads the persisted set.  Returns false when there is no port at all; the
    // store stays usable and empty rather than leaving the star dead.
    bool load();

    bool is_favourite(std::uint32_t id) const noexcept;

    // Returns the new state.  When the port cannot store it, the in-memory set
    // is put back the way it was: the star must never show something that would
    // be gone after a reboot.
    bool toggle_favourite(std::uint32_t id);

    const std::vector<std::uint32_t>& favourites() const noexcept { return favourites_; }

    // False when the last toggle was applied but not persisted (no port, a full
    // set, or a write that failed).
    bool last_write_ok() const noexcept { return last_write_ok_; }

private:
    static constexpr std::size_t kMaxFavourites = 512;

    LibraryStorePort port_{};
    std::vector<std::uint32_t> favourites_;
    bool last_write_ok_ = true;
};

#ifdef ESP_PLATFORM
// NVS-backed port: one blob in a small NVS namespace.  Never touches the card.
LibraryStorePort make_nvs_library_store_port();
#endif

}  // namespace media
