// NVS-backed LibraryStorePort.
//
// Kept in its own translation unit so the store itself stays host-buildable:
// this file compiles to nothing without ESP_PLATFORM.

#include "media/library/library_store.h"

#ifdef ESP_PLATFORM

#include <nvs.h>

namespace media {
namespace {

// Short namespace name (NVS allows 15 characters) and one blob, so a favourite
// toggle costs a single small write.
constexpr char kNamespace[] = "media_fav";
constexpr char kKey[] = "ids";

std::size_t nvs_load(void* /*context*/, std::uint32_t* ids, std::size_t capacity) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    std::size_t bytes = capacity * sizeof(std::uint32_t);
    const esp_err_t result = nvs_get_blob(handle, kKey, ids, &bytes);
    nvs_close(handle);
    if (result != ESP_OK) {
        return 0;
    }
    return bytes / sizeof(std::uint32_t);
}

bool nvs_save(void* /*context*/, const std::uint32_t* ids, std::size_t count) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t result = ESP_OK;
    if (count == 0) {
        result = nvs_erase_key(handle, kKey);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;  // nothing stored yet is not a failure
        }
    } else {
        result = nvs_set_blob(handle, kKey, ids, count * sizeof(std::uint32_t));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

}  // namespace

LibraryStorePort make_nvs_library_store_port() {
    return LibraryStorePort{nullptr, &nvs_load, &nvs_save};
}

}  // namespace media

#endif  // ESP_PLATFORM
