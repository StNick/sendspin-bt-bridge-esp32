#include "nvs_persistence.h"

#include "esp_log.h"
#include "nvs.h"

namespace {
const char* TAG = "persist";
const char* NVS_NAMESPACE = "sendspin";
}  // namespace

std::optional<std::vector<uint8_t>> NvsPersistenceProvider::load_blob(const std::string& key) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return std::nullopt;  // namespace not created yet: nothing stored
    }
    size_t len = 0;
    std::optional<std::vector<uint8_t>> result;
    if (nvs_get_blob(h, key.c_str(), nullptr, &len) == ESP_OK) {
        std::vector<uint8_t> bytes(len);
        if (len == 0 || nvs_get_blob(h, key.c_str(), bytes.data(), &len) == ESP_OK) {
            bytes.resize(len);
            result = std::move(bytes);
        }
    }
    nvs_close(h);
    return result;
}

bool NvsPersistenceProvider::save_blob(const std::string& key, const uint8_t* data, size_t len) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key.c_str(), data, len);
        // Write-through: commit here so every accepted write is durable on return.
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsPersistenceProvider::commit() {
    return true;  // save_blob() already committed
}
