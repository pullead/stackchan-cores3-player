// Host tests for the favourite store and the track identity it is keyed by.
//
// The FNV-1a literals below were produced by an independent implementation of
// the same hash, so a wrong basis, prime or fold order shows up here rather than
// as silently split favourites on the device.

#include "media/library/library_store.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace media;

namespace {

struct FakeStorage {
    std::vector<std::uint32_t> ids;
    bool fail_save = false;
};

std::size_t fake_load(void* context, std::uint32_t* ids, std::size_t capacity) {
    auto* storage = static_cast<FakeStorage*>(context);
    const std::size_t count = std::min(storage->ids.size(), capacity);
    std::copy(storage->ids.begin(), storage->ids.begin() + static_cast<std::ptrdiff_t>(count), ids);
    return count;
}

bool fake_save(void* context, const std::uint32_t* ids, std::size_t count) {
    auto* storage = static_cast<FakeStorage*>(context);
    if (storage->fail_save) {
        return false;
    }
    storage->ids.assign(ids, ids + count);
    return true;
}

LibraryStorePort port_for(FakeStorage& storage) {
    return LibraryStorePort{&storage, &fake_load, &fake_save};
}

}  // namespace

int main() {
    // Identity: stable, case-folded for ASCII, and sensitive to the path.
    {
        assert(make_track_id("/audiofiles/a.mp3") == 0xD5961DF3u);
        assert(make_track_id("/audiofiles/A.MP3") == make_track_id("/audiofiles/a.mp3"));
        assert(make_track_id("/Music/song.mp3") == 0x5DCE6BD7u);
        assert(make_track_id("/music/song.mp3") == make_track_id("/Music/song.mp3"));
        assert(make_track_id("/audiofiles/b.mp3") == 0x79089152u);
        assert(make_track_id("/audiofiles/01 - Track.mp3") == 0x01FA83EBu);
        assert(make_track_id("/audiofiles/a.mp3") != make_track_id("/audiofiles/b.mp3"));
        // No bytes hashed leaves the FNV basis itself.
        assert(make_track_id("") == 2166136261u);
    }

    // Without a port the star still works for the session, but nothing is
    // claimed to be stored.
    {
        LibraryStore store;
        assert(!store.ready());
        assert(!store.load());
        assert(store.favourites().empty());
        assert(store.toggle_favourite(42));
        assert(store.is_favourite(42));
        assert(!store.last_write_ok());
        assert(!store.toggle_favourite(42));
        assert(!store.is_favourite(42));
    }

    // With a port, a toggle round-trips through storage.
    {
        FakeStorage storage;
        LibraryStore store(port_for(storage));
        assert(store.ready());
        assert(store.load());
        assert(store.favourites().empty());

        assert(store.toggle_favourite(7));
        assert(store.last_write_ok());
        assert(store.is_favourite(7));
        assert(storage.ids.size() == 1 && storage.ids[0] == 7);

        // A second store reading the same storage sees the same set.
        LibraryStore reopened(port_for(storage));
        assert(reopened.load());
        assert(reopened.is_favourite(7));
        assert(reopened.favourites().size() == 1);

        assert(!store.toggle_favourite(7));
        assert(!store.is_favourite(7));
        assert(storage.ids.empty());
    }

    // A failed write must not leave the star claiming something that was not
    // stored: the in-memory set goes back and the caller is told.
    {
        FakeStorage storage;
        LibraryStore store(port_for(storage));
        assert(store.load());

        storage.fail_save = true;
        assert(!store.toggle_favourite(11));
        assert(!store.is_favourite(11));
        assert(!store.last_write_ok());
        assert(store.favourites().empty());

        // Un-favouriting an id that is only in memory is not a thing; make one
        // stored first, then fail the removal.
        storage.fail_save = false;
        assert(store.toggle_favourite(11));
        assert(store.is_favourite(11));

        storage.fail_save = true;
        assert(store.toggle_favourite(11));
        assert(store.is_favourite(11));
        assert(!store.last_write_ok());
        assert(storage.ids.size() == 1 && storage.ids[0] == 11);
    }

    // load() also resets the "last write" flag, so a stale failure from an
    // earlier session does not linger in the UI.
    {
        FakeStorage storage;
        LibraryStore store(port_for(storage));
        store.toggle_favourite(3);
        assert(store.last_write_ok());
        assert(store.load());
        assert(store.last_write_ok());
    }

    // The view filter: indices of the favourites, in library order.
    {
        FakeStorage storage;
        LibraryStore store(port_for(storage));
        store.load();
        const std::vector<std::string> paths = {
            "/audiofiles/a.mp3",
            "/audiofiles/b.mp3",
            "/audiofiles/c.mp3",
            "/Music/D.MP3",
        };
        assert(favourite_indices(paths, store).empty());

        // Toggled out of order: the result must still come back in library
        // order, because that is the order the list shows.
        store.toggle_favourite(make_track_id(paths[2]));
        store.toggle_favourite(make_track_id(paths[0]));
        const auto picked = favourite_indices(paths, store);
        assert(picked.size() == 2);
        assert(picked[0] == 0);
        assert(picked[1] == 2);

        // A path differing only in ASCII case is the same track, which is the
        // point of folding before hashing.
        store.toggle_favourite(make_track_id("/music/d.mp3"));
        const auto with_case = favourite_indices(paths, store);
        assert(with_case.size() == 3);
        assert(with_case[2] == 3);
    }

    // A store with no port has no favourites, so the ★ view is empty rather
    // than showing a phantom row.
    {
        LibraryStore store;
        const std::vector<std::string> paths = {"/audiofiles/a.mp3"};
        assert(favourite_indices(paths, store).empty());
    }

    return 0;
}
