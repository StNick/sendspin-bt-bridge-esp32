#include "web_server.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "app_state.h"
#include "bt_manager.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "log_buffer.h"
#include "mbedtls/base64.h"
#include "wifi_manager.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

namespace {

const char* TAG = "web";
constexpr size_t MAX_JSON_BODY = 4096;
constexpr int MAX_RECV_TIMEOUTS = 3;     // x recv_wait_timeout (10 s)
constexpr size_t MAX_WEB_PASSWORD = 64;  // keeps "admin:<pw>" well inside the base64 buffer

WebActions g_actions;
httpd_handle_t g_server = nullptr;
esp_timer_handle_t g_reboot_timer = nullptr;

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

/// HTTP Basic auth (user "admin") when a web password is set. Returns false after sending 401.
bool authorized(httpd_req_t* req) {
    const std::string password = app_config::get().web_password;
    if (password.empty()) {
        return true;
    }
    const std::string credentials = "admin:" + password;
    unsigned char encoded[256];
    size_t encoded_len = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                              reinterpret_cast<const unsigned char*>(credentials.data()),
                              credentials.size()) == 0) {
        const std::string expected = "Basic " + std::string(reinterpret_cast<char*>(encoded), encoded_len);
        size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
        if (header_len > 0 && header_len < 512) {
            std::string header(header_len + 1, '\0');
            if (httpd_req_get_hdr_value_str(req, "Authorization", header.data(), header.size()) == ESP_OK) {
                header.resize(header_len);
                if (constant_time_equal(header, expected)) {
                    return true;
                }
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Sendspin BT Bridge\"");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return false;
}

esp_err_t send_json(httpd_req_t* req, const JsonDocument& doc) {
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), static_cast<ssize_t>(out.size()));
}

esp_err_t send_ok(httpd_req_t* req) {
    JsonDocument doc;
    doc["ok"] = true;
    return send_json(req, doc);
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* message) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = message;
    httpd_resp_set_status(req, status);
    return send_json(req, doc);
}

/// Reads a JSON body into doc. Sends an error and returns false on failure.
bool read_json(httpd_req_t* req, JsonDocument& doc) {
    if (req->content_len == 0) {
        doc.to<JsonObject>();
        return true;
    }
    if (req->content_len > MAX_JSON_BODY) {
        send_error(req, "413 Payload Too Large", "body too large");
        return false;
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    int timeouts = 0;
    while (got < body.size()) {
        int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= MAX_RECV_TIMEOUTS) {
            continue;  // bounded: a stalled client must not hold the single httpd task
        }
        if (r <= 0) {
            send_error(req, "400 Bad Request", "body read failed");
            return false;
        }
        got += static_cast<size_t>(r);
    }
    if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        send_error(req, "400 Bad Request", "invalid JSON");
        return false;
    }
    return true;
}

std::atomic<bool> g_erase_before_reboot{false};

void reboot_cb(void*) {
    if (g_erase_before_reboot.load()) {
        // Erase at the last moment so nothing (Bluedroid, Wi-Fi, Sendspin persistence) runs
        // against a de-initialised NVS in between.
        app_config::factory_reset();
    }
    esp_restart();
}

void schedule_reboot() {
    if (g_reboot_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = reboot_cb;
        args.name = "reboot";
        esp_timer_create(&args, &g_reboot_timer);
    }
    esp_timer_stop(g_reboot_timer);
    esp_timer_start_once(g_reboot_timer, 1500000);  // let the HTTP response go out first
}

// ------------------------------------------------------------------------------------------
// Pages
// ------------------------------------------------------------------------------------------

esp_err_t handle_index(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // EMBED_TXTFILES appends a NUL terminator; do not send it.
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

esp_err_t handle_status(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    const AppConfig cfg = app_config::get();
    const esp_app_desc_t* app = esp_app_get_description();

    JsonObject sys = doc["system"].to<JsonObject>();
    sys["version"] = app->version;
    sys["idf"] = app->idf_ver;
    sys["uptime_s"] = static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
    sys["heap_internal_free"] = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    sys["heap_internal_min"] = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    sys["psram_total"] = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    sys["psram_free"] = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    sys["name"] = cfg.name;
    sys["idle_mode"] = idle_mode_name(cfg.idle_mode);

    const WifiStatus w = wifi_manager::status();
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["configured"] = w.sta_configured;
    wifi["connected"] = w.sta_connected;
    wifi["ssid"] = w.ssid;
    wifi["ip"] = w.ip;
    wifi["rssi"] = w.rssi;
    wifi["ap_active"] = w.ap_active;
    wifi["ap_ssid"] = w.ap_ssid;
    wifi["hostname"] = w.hostname;
    wifi["mac"] = w.mac;

    const BtStatus b = BtManager::instance().status();
    JsonObject bt = doc["bluetooth"].to<JsonObject>();
    bt["ready"] = b.ready;
    bt["local_mac"] = b.local_mac;
    bt["state"] = b.state;
    bt["has_speaker"] = b.has_target;
    bt["speaker_mac"] = b.target_mac;
    bt["speaker_name"] = b.target_name;
    bt["connected"] = b.connected;
    bt["streaming"] = b.streaming;
    bt["power_saved"] = b.power_saved;
    bt["standby"] = b.standby;
    bt["gave_up"] = b.gave_up;
    bt["reconnect_attempt"] = b.reconnect_attempt;
    bt["next_attempt_in_s"] = b.next_attempt_in_s;
    bt["scanning"] = b.scanning;
    bt["abs_volume"] = b.abs_volume;
    if (b.rssi_valid) {
        bt["rssi_dbm"] = b.rssi_dbm;
    }
    if (b.delay_report_ms >= 0) {
        bt["delay_report_ms"] = b.delay_report_ms;
    }
    bt["mono_sink"] = b.mono_sink;
    bt["codec"] = b.codec;
    bt["bitrate_bps"] = b.bitrate_bps;
    bt["encoder_avg_us"] = b.encoder_metrics.encode_avg_us;
    bt["encoder_max_us"] = b.encoder_metrics.encode_max_us;
    bt["encoder_late_batches"] = b.encoder_metrics.late_batches;
    bt["send_retries"] = b.encoder_metrics.send_retries;
    bt["dropped_packets"] = b.encoder_metrics.dropped_packets;
    bt["last_event"] = b.last_event;
    JsonArray results = bt["scan_results"].to<JsonArray>();
    for (const auto& r : b.scan_results) {
        JsonObject o = results.add<JsonObject>();
        o["mac"] = app_config::bda_to_string(r.bda);
        o["name"] = r.name;
        o["rssi"] = r.rssi;
        o["audio"] = r.audio;
        o["bonded"] = r.bonded;
    }
    JsonArray bonded = bt["bonded"].to<JsonArray>();
    for (const auto& m : b.bonded) {
        bonded.add(m);
    }

    const app_state::SendspinStatus s = app_state::get_sendspin();
    JsonObject ss = doc["sendspin"].to<JsonObject>();
    ss["enabled"] = s.enabled;
    ss["disabled_reason"] = s.disabled_reason;
    ss["client_id"] = s.client_id;
    ss["connected"] = s.connected;
    ss["paired"] = s.paired;
    ss["time_synced"] = s.time_synced;
    ss["unpaired_access"] = s.unpaired_access;
    ss["server_name"] = s.server_name;
    ss["group_name"] = s.group_name;
    ss["playback_state"] = s.playback_state;
    ss["stream_active"] = s.stream_active;
    ss["stream_codec"] = s.stream_codec;
    ss["volume"] = s.volume;
    ss["muted"] = s.muted;
    ss["output_delay_ms"] = s.output_delay_ms;
    ss["title"] = s.title;
    ss["artist"] = s.artist;
    ss["album"] = s.album;
    ss["progress_ms"] = s.progress_ms;
    ss["duration_ms"] = s.duration_ms;
    ss["shuffle"] = s.shuffle;
    ss["repeat"] = s.repeat;
    JsonArray cmds = ss["supported_commands"].to<JsonArray>();
    for (const auto& c : s.supported_commands) {
        cmds.add(c);
    }
    ss["pairing_code"] = s.pairing_code;
    ss["pairing_window_prompt"] = s.pairing_window_prompt;
    ss["pairing_event"] = s.pairing_event;
    ss["idle_state"] = s.idle_state;

    return send_json(req, doc);
}

esp_err_t handle_get_config(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    const AppConfig c = app_config::get();
    JsonDocument doc;
    doc["name"] = c.name;
    doc["wifi_ssid"] = c.wifi_ssid;
    doc["wifi_password_set"] = !c.wifi_password.empty();
    doc["server_url"] = c.server_url;
    doc["require_pairing"] = c.require_pairing;
    doc["idle_mode"] = idle_mode_name(c.idle_mode);
    doc["power_save_delay_min"] = c.power_save_delay_min;
    doc["idle_disconnect_min"] = c.idle_disconnect_min;
    doc["keepalive_interval_s"] = c.keepalive_interval_s;
    doc["keepalive_method"] = keepalive_method_name(c.keepalive_method);
    doc["bt_check_interval_s"] = c.bt_check_interval_s;
    doc["bt_max_reconnect_fails"] = c.bt_max_reconnect_fails;
    doc["bt_pin"] = c.bt_pin;
    doc["ldac_quality"] = ldac_quality_name(c.ldac_quality);
    doc["min_buffer_ms"] = c.min_buffer_ms;
    doc["lead_time_ms"] = c.lead_time_ms;
    doc["web_password_set"] = !c.web_password.empty();
    return send_json(req, doc);
}

template <typename T>
bool read_uint(JsonObjectConst obj, const char* key, T min, T max, T* out, std::string* error) {
    JsonVariantConst v = obj[key];
    if (v.isNull()) {
        return true;
    }
    if (!v.is<long long>()) {
        *error = std::string(key) + " must be an integer";
        return false;
    }
    long long n = v.as<long long>();
    if (n < static_cast<long long>(min) || n > static_cast<long long>(max)) {
        *error = std::string(key) + " out of range";
        return false;
    }
    *out = static_cast<T>(n);
    return true;
}

esp_err_t handle_post_config(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    if (!read_json(req, doc)) {
        return ESP_OK;
    }
    JsonObjectConst in = doc.as<JsonObjectConst>();
    const AppConfig before = app_config::get();
    AppConfig c = before;
    std::string error;

    if (in["name"].is<const char*>()) {
        std::string name = in["name"].as<std::string>();
        if (name.empty() || name.size() > 63) {
            return send_error(req, "400 Bad Request", "name must be 1-63 characters");
        }
        c.name = name;
    }
    if (in["wifi_ssid"].is<const char*>()) {
        c.wifi_ssid = in["wifi_ssid"].as<std::string>();
        if (c.wifi_ssid.size() > 32) {
            return send_error(req, "400 Bad Request", "wifi_ssid longer than 32 bytes");
        }
    }
    if (in["wifi_password"].is<const char*>()) {
        c.wifi_password = in["wifi_password"].as<std::string>();
        if (!c.wifi_password.empty() && (c.wifi_password.size() < 8 || c.wifi_password.size() > 63)) {
            return send_error(req, "400 Bad Request", "wifi_password must be 8-63 characters");
        }
    }
    if (in["server_url"].is<const char*>()) {
        std::string url = in["server_url"].as<std::string>();
        if (!url.empty() && url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
            return send_error(req, "400 Bad Request", "server_url must start with ws:// or wss://");
        }
        c.server_url = url;
    }
    if (in["require_pairing"].is<bool>()) {
        c.require_pairing = in["require_pairing"].as<bool>();
    }
    if (!in["ldac_quality"].isNull()) {
        if (!in["ldac_quality"].is<const char*>() ||
            !ldac_quality_from_name(in["ldac_quality"].as<std::string>(), &c.ldac_quality)) {
            return send_error(req, "400 Bad Request", "unknown ldac_quality");
        }
    }
    if (in["idle_mode"].is<const char*>()) {
        if (!idle_mode_from_name(in["idle_mode"].as<std::string>(), &c.idle_mode)) {
            return send_error(req, "400 Bad Request", "unknown idle_mode");
        }
    }
    if (in["keepalive_method"].is<const char*>()) {
        if (!keepalive_method_from_name(in["keepalive_method"].as<std::string>(), &c.keepalive_method)) {
            return send_error(req, "400 Bad Request", "unknown keepalive_method");
        }
    }
    if (!read_uint<uint16_t>(in, "power_save_delay_min", 0, 1440, &c.power_save_delay_min, &error) ||
        !read_uint<uint16_t>(in, "idle_disconnect_min", 0, 1440, &c.idle_disconnect_min, &error) ||
        !read_uint<uint16_t>(in, "keepalive_interval_s", 30, 3600, &c.keepalive_interval_s, &error) ||
        !read_uint<uint16_t>(in, "bt_check_interval_s", 5, 600, &c.bt_check_interval_s, &error) ||
        !read_uint<uint16_t>(in, "bt_max_reconnect_fails", 0, 1000, &c.bt_max_reconnect_fails, &error) ||
        !read_uint<uint16_t>(in, "min_buffer_ms", 100, 3000, &c.min_buffer_ms, &error) ||
        !read_uint<uint16_t>(in, "lead_time_ms", 0, 5000, &c.lead_time_ms, &error)) {
        return send_error(req, "400 Bad Request", error.c_str());
    }
    if (in["bt_pin"].is<const char*>()) {
        std::string pin = in["bt_pin"].as<std::string>();
        if (pin.empty() || pin.size() > 16) {
            return send_error(req, "400 Bad Request", "bt_pin must be 1-16 characters");
        }
        c.bt_pin = pin;
    }
    if (in["web_password"].is<const char*>()) {
        std::string pw = in["web_password"].as<std::string>();
        if (pw.size() > MAX_WEB_PASSWORD) {
            return send_error(req, "400 Bad Request", "web_password must be at most 64 characters");
        }
        c.web_password = pw;
    }

    // Apply only the fields this form owns, under the settings lock, so a concurrent update
    // (speaker selection, volume save) is not overwritten with a stale copy.
    esp_err_t save_err = app_config::update([&c](AppConfig& cur) {
        cur.name = c.name;
        cur.wifi_ssid = c.wifi_ssid;
        cur.wifi_password = c.wifi_password;
        cur.server_url = c.server_url;
        cur.require_pairing = c.require_pairing;
        cur.idle_mode = c.idle_mode;
        cur.keepalive_method = c.keepalive_method;
        cur.power_save_delay_min = c.power_save_delay_min;
        cur.idle_disconnect_min = c.idle_disconnect_min;
        cur.keepalive_interval_s = c.keepalive_interval_s;
        cur.bt_check_interval_s = c.bt_check_interval_s;
        cur.bt_max_reconnect_fails = c.bt_max_reconnect_fails;
        cur.min_buffer_ms = c.min_buffer_ms;
        cur.lead_time_ms = c.lead_time_ms;
        cur.bt_pin = c.bt_pin;
        cur.ldac_quality = c.ldac_quality;
        cur.web_password = c.web_password;
    });
    if (save_err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "saving settings failed");
    }
    const AppConfig after = app_config::get();
    const bool reboot_required = before.name != after.name || before.wifi_ssid != after.wifi_ssid ||
                                 before.wifi_password != after.wifi_password ||
                                 before.server_url != after.server_url ||
                                 before.min_buffer_ms != after.min_buffer_ms ||
                                 before.lead_time_ms != after.lead_time_ms;
    if (g_actions.settings_changed) {
        g_actions.settings_changed(before, after);
    }
    JsonDocument out;
    out["ok"] = true;
    out["reboot_required"] = reboot_required;
    return send_json(req, out);
}

esp_err_t handle_logs(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    std::string text = log_buffer::dump();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text.data(), static_cast<ssize_t>(text.size()));
}

esp_err_t handle_wifi_scan(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (const auto& n : wifi_manager::scan()) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = n.ssid;
        o["rssi"] = n.rssi;
        o["secure"] = n.secure;
    }
    return send_json(req, doc);
}

esp_err_t handle_bt_scan(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    BtManager::instance().start_scan();
    return send_ok(req);
}

esp_err_t handle_bt_select(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    if (!read_json(req, doc)) {
        return ESP_OK;
    }
    std::array<uint8_t, 6> bda{};
    if (!app_config::bda_from_string(doc["mac"] | std::string(), &bda)) {
        return send_error(req, "400 Bad Request", "mac must be XX:XX:XX:XX:XX:XX");
    }
    std::string name = doc["name"] | std::string();
    if (name.size() > 63) {
        name.resize(63);
    }
    bool changed = false;
    esp_err_t err = app_config::update([&](AppConfig& c) {
        changed = !c.has_speaker || c.speaker_bda != bda;
        c.has_speaker = true;
        c.speaker_bda = bda;
        c.speaker_name = name;
        if (changed) {
            c.delay_auto_pending = true;
        }
    });
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "saving settings failed");
    }
    if (changed && g_actions.speaker_selected) {
        g_actions.speaker_selected();
    }
    BtManager::instance().set_target(bda, name);
    return send_ok(req);
}

esp_err_t handle_bt_forget(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    app_config::update([](AppConfig& c) {
        c.has_speaker = false;
        c.speaker_name.clear();
        c.speaker_bda.fill(0);
    });
    BtManager::instance().clear_target(true);
    return send_ok(req);
}

esp_err_t handle_bt_reconnect(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    BtManager::instance().reconnect_now();
    return send_ok(req);
}

esp_err_t handle_bt_standby(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    BtManager::instance().enter_standby();
    return send_ok(req);
}

esp_err_t handle_transport(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    if (!read_json(req, doc)) {
        return ESP_OK;
    }
    const std::string command = doc["command"] | std::string();
    if (!g_actions.transport || !g_actions.transport(command)) {
        return send_error(req, "409 Conflict", "command not available now");
    }
    return send_ok(req);
}

esp_err_t handle_volume(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    if (!read_json(req, doc)) {
        return ESP_OK;
    }
    if (doc["volume"].is<long long>()) {
        long long v = doc["volume"].as<long long>();
        if (v < 0 || v > 100) {
            return send_error(req, "400 Bad Request", "volume must be 0-100");
        }
        if (g_actions.set_volume) {
            g_actions.set_volume(static_cast<uint8_t>(v));
        }
    }
    if (doc["muted"].is<bool>() && g_actions.set_muted) {
        g_actions.set_muted(doc["muted"].as<bool>());
    }
    return send_ok(req);
}

esp_err_t handle_delay(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    JsonDocument doc;
    if (!read_json(req, doc)) {
        return ESP_OK;
    }
    if (!doc["delay_ms"].is<long long>()) {
        return send_error(req, "400 Bad Request", "delay_ms required");
    }
    long long ms = doc["delay_ms"].as<long long>();
    if (ms < 0 || ms > 5000) {
        return send_error(req, "400 Bad Request", "delay_ms must be 0-5000");
    }
    if (g_actions.set_delay) {
        g_actions.set_delay(static_cast<uint16_t>(ms));
    }
    return send_ok(req);
}

esp_err_t handle_pairing_confirm(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (g_actions.pairing_confirm) {
        g_actions.pairing_confirm();
    }
    return send_ok(req);
}

esp_err_t handle_pairing_cancel(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (g_actions.pairing_cancel) {
        g_actions.pairing_cancel();
    }
    return send_ok(req);
}

esp_err_t handle_reboot(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    schedule_reboot();
    return send_ok(req);
}

esp_err_t handle_factory_reset(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    send_ok(req);
    ESP_LOGW(TAG, "Factory reset requested");
    g_erase_before_reboot.store(true);
    schedule_reboot();
    return ESP_OK;
}

esp_err_t handle_ota(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        return send_error(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return send_error(req, "400 Bad Request", "firmware size invalid");
    }
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    constexpr size_t CHUNK = 4096;
    char* buf = static_cast<char*>(heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        esp_ota_abort(ota);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t remaining = req->content_len;
    int timeouts = 0;
    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", static_cast<unsigned>(remaining), target->label);
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, std::min(remaining, CHUNK));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= MAX_RECV_TIMEOUTS) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, static_cast<size_t>(r));
        if (err != ESP_OK) {
            break;
        }
        remaining -= static_cast<size_t>(r);
    }
    heap_caps_free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        return send_error(req, "500 Internal Server Error", "upload or flash write failed");
    }
    err = esp_ota_end(ota);  // validates the image
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "image validation failed");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA complete; rebooting into %s", target->label);
    send_ok(req);
    schedule_reboot();
    return ESP_OK;
}

void register_uri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    esp_err_t err = httpd_register_uri_handler(g_server, &u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
    }
}

}  // namespace

namespace web_server {

esp_err_t start(WebActions actions) {
    g_actions = std::move(actions);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    config.max_open_sockets = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    register_uri("/", HTTP_GET, handle_index);
    register_uri("/api/status", HTTP_GET, handle_status);
    register_uri("/api/config", HTTP_GET, handle_get_config);
    register_uri("/api/config", HTTP_POST, handle_post_config);
    register_uri("/api/logs", HTTP_GET, handle_logs);
    register_uri("/api/wifi/scan", HTTP_GET, handle_wifi_scan);
    register_uri("/api/bt/scan", HTTP_POST, handle_bt_scan);
    register_uri("/api/bt/select", HTTP_POST, handle_bt_select);
    register_uri("/api/bt/forget", HTTP_POST, handle_bt_forget);
    register_uri("/api/bt/reconnect", HTTP_POST, handle_bt_reconnect);
    register_uri("/api/bt/standby", HTTP_POST, handle_bt_standby);
    register_uri("/api/transport", HTTP_POST, handle_transport);
    register_uri("/api/volume", HTTP_POST, handle_volume);
    register_uri("/api/delay", HTTP_POST, handle_delay);
    register_uri("/api/pairing/confirm", HTTP_POST, handle_pairing_confirm);
    register_uri("/api/pairing/cancel", HTTP_POST, handle_pairing_cancel);
    register_uri("/api/reboot", HTTP_POST, handle_reboot);
    register_uri("/api/factory_reset", HTTP_POST, handle_factory_reset);
    register_uri("/api/ota", HTTP_POST, handle_ota);
    ESP_LOGI(TAG, "Web UI on port 80");
    return ESP_OK;
}

}  // namespace web_server
