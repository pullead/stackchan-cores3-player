#include "media/library/library_store.h"

#include <algorithm>

namespace media {

std::uint32_t make_track_id(std::string_view path) noexcept {
    // FNV-1a, 32 bit.  Bytes above ASCII are hashed as they are: the path is
    // UTF-8 and only the letters that have an unambiguous lower case are folded.
    std::uint32_t hash = 2166136261u;
    for (char character : path) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

bool LibraryStore::load() {
    favourites_.clear();
    last_write_ok_ = true;
    if (!ready()) {
        return false;
    }

    std::vector<std::uint32_t> buffer(kMaxFavourites);
    const std::size_t count = port_.load(port_.context, buffer.data(), buffer.size());
    const std::size_t accepted = std::min(count, buffer.size());
    favourites_.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(accepted));
    return true;
}

bool LibraryStore::is_favourite(std::uint32_t id) const noexcept {
    return std::find(favourites_.begin(), favourites_.end(), id) != favourites_.end();
}

bool LibraryStore::toggle_favourite(std::uint32_t id) {
    const bool was_favourite = is_favourite(id);
    if (!was_favourite && favourites_.size() >= kMaxFavourites) {
        last_write_ok_ = false;
        return false;
    }

    if (was_favourite) {
        favourites_.erase(std::remove(favourites_.begin(), favourites_.end(), id),
                          favourites_.end());
    } else {
        favourites_.push_back(id);
    }

    if (!ready()) {
        // Applied for this session only; say so instead of pretending it saved.
        last_write_ok_ = false;
        return !was_favourite;
    }

    if (!port_.save(port_.context, favourites_.data(), favourites_.size())) {
        if (was_favourite) {
            favourites_.push_back(id);
        } else {
            favourites_.pop_back();
        }
        last_write_ok_ = false;
        return was_favourite;
    }

    last_write_ok_ = true;
    return !was_favourite;
}

}  // namespace media
