#include "app_config.h"

#include <cctype>
#include <cstdio>
#include <mutex>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

const char* TAG = "config";
const char* NVS_NAMESPACE = "bridge";

std::mutex g_mutex;
AppConfig g_config;

std::string read_str(nvs_handle_t h, const char* key, const std::string& fallback) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) {
        return fallback;
    }
    std::string value(len, '\0');
    if (nvs_get_str(h, key, value.data(), &len) != ESP_OK) {
        return fallback;
    }
    value.resize(len - 1);  // drop the stored NUL terminator
    return value;
}

uint8_t read_u8(nvs_handle_t h, const char* key, uint8_t fallback) {
    uint8_t v = fallback;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v : fallback;
}

uint16_t read_u16(nvs_handle_t h, const char* key, uint16_t fallback) {
    uint16_t v = fallback;
    return nvs_get_u16(h, key, &v) == ESP_OK ? v : fallback;
}

std::string default_name() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "Sendspin BT %02X%02X", mac[4], mac[5]);
    return buf;
}

void sanitize(AppConfig& c) {
    if (c.name.empty()) {
        c.name = default_name();
    }
    if (c.name.size() > 63) {
        c.name.resize(63);
    }
    if (c.keepalive_interval_s < 30) {
        c.keepalive_interval_s = 30;
    }
    if (c.bt_check_interval_s < 5) {
        c.bt_check_interval_s = 5;
    }
    if (c.bt_pin.empty() || c.bt_pin.size() > 16) {
        c.bt_pin = "0000";
    }
    if (static_cast<uint8_t>(c.ldac_quality) > static_cast<uint8_t>(LdacQuality::CONNECTION)) {
        c.ldac_quality = LdacQuality::STANDARD;
    }
    if (static_cast<uint8_t>(c.idle_mode) > static_cast<uint8_t>(IdleMode::KEEP_ALIVE)) {
        c.idle_mode = IdleMode::DEFAULT;
    }
    if (static_cast<uint8_t>(c.keepalive_method) > static_cast<uint8_t>(KeepAliveMethod::NONE)) {
        c.keepalive_method = KeepAliveMethod::INFRASOUND;
    }
    if (c.last_volume > 100) {
        c.last_volume = 100;
    }
    if (c.min_buffer_ms < 100) {
        c.min_buffer_ms = 100;
    }
    if (c.audio_sample_rate != 44100 && c.audio_sample_rate != 48000) c.audio_sample_rate = 44100;
    if (c.audio_bit_depth != 16 && c.audio_bit_depth != 24) c.audio_bit_depth = 16;
    // min_buffer_ms must fit sendspin-cpp's advertised buffer (621,312 bytes with the default
    // 1 MB capacity). At 48 kHz stereo, 3000 ms of 16-bit or 2000 ms of 24-bit
    // PCM requires 576,000 bytes.
    const uint16_t max_buffer_ms = c.audio_bit_depth == 24 ? 2000 : 3000;
    if (c.min_buffer_ms > max_buffer_ms) {
        c.min_buffer_ms = max_buffer_ms;
    }
    if (c.lead_time_ms > 5000) {
        c.lead_time_ms = 5000;
    }
}

esp_err_t write_all(const AppConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    // Collect the first error but still attempt every key.
    esp_err_t first = ESP_OK;
    auto check = [&first](esp_err_t e) {
        if (e != ESP_OK && first == ESP_OK) {
            first = e;
        }
    };
    check(nvs_set_str(h, "wifi_ssid", c.wifi_ssid.c_str()));
    check(nvs_set_str(h, "wifi_pass", c.wifi_password.c_str()));
    check(nvs_set_str(h, "name", c.name.c_str()));
    check(nvs_set_u8(h, "spk_set", c.has_speaker ? 1 : 0));
    check(nvs_set_blob(h, "spk_bda", c.speaker_bda.data(), c.speaker_bda.size()));
    check(nvs_set_str(h, "spk_name", c.speaker_name.c_str()));
    check(nvs_set_str(h, "bt_pin", c.bt_pin.c_str()));
    check(nvs_set_u8(h, "ldac_quality", static_cast<uint8_t>(c.ldac_quality)));
    check(nvs_set_u16(h, "audio_rate", static_cast<uint16_t>(c.audio_sample_rate)));
    check(nvs_set_u8(h, "audio_bits", c.audio_bit_depth));
    check(nvs_set_u8(h, "idle_mode", static_cast<uint8_t>(c.idle_mode)));
    check(nvs_set_u16(h, "ps_delay", c.power_save_delay_min));
    check(nvs_set_u16(h, "idle_disc", c.idle_disconnect_min));
    check(nvs_set_u16(h, "ka_int", c.keepalive_interval_s));
    check(nvs_set_u8(h, "ka_method", static_cast<uint8_t>(c.keepalive_method)));
    check(nvs_set_u16(h, "bt_check", c.bt_check_interval_s));
    check(nvs_set_u16(h, "bt_maxfail", c.bt_max_reconnect_fails));
    check(nvs_set_str(h, "srv_url", c.server_url.c_str()));
    check(nvs_set_u8(h, "req_pair", c.require_pairing ? 1 : 0));
    check(nvs_set_u16(h, "min_buf", c.min_buffer_ms));
    check(nvs_set_u16(h, "lead_ms", c.lead_time_ms));
    check(nvs_set_u8(h, "dly_auto", c.delay_auto_pending ? 1 : 0));
    check(nvs_set_u8(h, "vol_set", c.has_last_volume ? 1 : 0));
    check(nvs_set_u8(h, "vol", c.last_volume));
    check(nvs_set_str(h, "web_pass", c.web_password.c_str()));
    check(nvs_commit(h));
    nvs_close(h);
    return first;
}

}  // namespace

const char* idle_mode_name(IdleMode mode) {
    switch (mode) {
        case IdleMode::POWER_SAVE:
            return "power_save";
        case IdleMode::AUTO_DISCONNECT:
            return "auto_disconnect";
        case IdleMode::KEEP_ALIVE:
            return "keep_alive";
        case IdleMode::DEFAULT:
        default:
            return "default";
    }
}

const char* ldac_quality_name(LdacQuality quality) {
    switch (quality) {
        case LdacQuality::STANDARD: return "standard";
        case LdacQuality::CONNECTION: return "connection";
        default: return "high";
    }
}

bool ldac_quality_from_name(const std::string& name, LdacQuality* out) {
    if (name == "high") *out = LdacQuality::HIGH;
    else if (name == "standard") *out = LdacQuality::STANDARD;
    else if (name == "connection") *out = LdacQuality::CONNECTION;
    else return false;
    return true;
}

bool idle_mode_from_name(const std::string& name, IdleMode* out) {
    if (name == "default") {
        *out = IdleMode::DEFAULT;
    } else if (name == "power_save") {
        *out = IdleMode::POWER_SAVE;
    } else if (name == "auto_disconnect") {
        *out = IdleMode::AUTO_DISCONNECT;
    } else if (name == "keep_alive") {
        *out = IdleMode::KEEP_ALIVE;
    } else {
        return false;
    }
    return true;
}

const char* keepalive_method_name(KeepAliveMethod method) {
    switch (method) {
        case KeepAliveMethod::SILENCE:
            return "silence";
        case KeepAliveMethod::NONE:
            return "none";
        case KeepAliveMethod::INFRASOUND:
        default:
            return "infrasound";
    }
}

bool keepalive_method_from_name(const std::string& name, KeepAliveMethod* out) {
    if (name == "infrasound") {
        *out = KeepAliveMethod::INFRASOUND;
    } else if (name == "silence") {
        *out = KeepAliveMethod::SILENCE;
    } else if (name == "none") {
        *out = KeepAliveMethod::NONE;
    } else {
        return false;
    }
    return true;
}

namespace app_config {

esp_err_t init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout incompatible (%s); erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    AppConfig c;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        c.wifi_ssid = read_str(h, "wifi_ssid", "");
        c.wifi_password = read_str(h, "wifi_pass", "");
        c.name = read_str(h, "name", "");
        c.has_speaker = read_u8(h, "spk_set", 0) != 0;
        size_t len = c.speaker_bda.size();
        if (nvs_get_blob(h, "spk_bda", c.speaker_bda.data(), &len) != ESP_OK ||
            len != c.speaker_bda.size()) {
            c.has_speaker = false;
            c.speaker_bda.fill(0);
        }
        c.speaker_name = read_str(h, "spk_name", "");
        c.bt_pin = read_str(h, "bt_pin", "0000");
        c.ldac_quality = static_cast<LdacQuality>(read_u8(h, "ldac_quality", 1));
        c.audio_sample_rate = read_u16(h, "audio_rate", 44100);
        c.audio_bit_depth = read_u8(h, "audio_bits", 16);
        c.idle_mode = static_cast<IdleMode>(read_u8(h, "idle_mode", 0));
        c.power_save_delay_min = read_u16(h, "ps_delay", 1);
        c.idle_disconnect_min = read_u16(h, "idle_disc", 0);
        c.keepalive_interval_s = read_u16(h, "ka_int", 30);
        c.keepalive_method = static_cast<KeepAliveMethod>(read_u8(h, "ka_method", 0));
        c.bt_check_interval_s = read_u16(h, "bt_check", 10);
        c.bt_max_reconnect_fails = read_u16(h, "bt_maxfail", 0);
        c.server_url = read_str(h, "srv_url", "");
        c.require_pairing = read_u8(h, "req_pair", 0) != 0;
        c.min_buffer_ms = read_u16(h, "min_buf", 500);
        c.lead_time_ms = read_u16(h, "lead_ms", 250);
        c.delay_auto_pending = read_u8(h, "dly_auto", 1) != 0;
        c.has_last_volume = read_u8(h, "vol_set", 0) != 0;
        c.last_volume = read_u8(h, "vol", 50);
        c.web_password = read_str(h, "web_pass", "");
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "No stored settings; using defaults");
    }
    sanitize(c);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = c;
    return ESP_OK;
}

AppConfig get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config;
}

esp_err_t set(const AppConfig& config) {
    AppConfig c = config;
    sanitize(c);
    std::lock_guard<std::mutex> lock(g_mutex);
    esp_err_t err = write_all(c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
    }
    g_config = c;  // in-memory copy stays authoritative for this boot either way
    return err;
}

esp_err_t update(const std::function<void(AppConfig&)>& mutate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    AppConfig c = g_config;
    mutate(c);
    sanitize(c);
    esp_err_t err = write_all(c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
    }
    g_config = c;
    return err;
}

esp_err_t factory_reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    nvs_flash_deinit();
    return nvs_flash_erase();
}

std::string hostname_from_name(const std::string& name) {
    std::string out;
    bool last_dash = true;
    for (char ch : name) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
        if (out.size() >= 32) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "sendspin-bt";
    }
    return out;
}

std::string bda_to_string(const std::array<uint8_t, 6>& bda) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3],
             bda[4], bda[5]);
    return buf;
}

bool bda_from_string(const std::string& text, std::array<uint8_t, 6>* out) {
    unsigned int b[6];
    char trailing;
    if (sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
               &trailing) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        (*out)[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

}  // namespace app_config
