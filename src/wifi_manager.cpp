#include "wifi_manager.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

namespace {

const char* TAG = "wifi";
const char* AP_PASSWORD = "sendspin";
constexpr int64_t AP_FALLBACK_US = 60LL * 1000000LL;

std::mutex g_mutex;
esp_netif_t* g_sta = nullptr;
esp_netif_t* g_ap = nullptr;
std::atomic<bool> g_connected{false};
bool g_sta_configured = false;
bool g_ap_active = false;
std::string g_ssid;
std::string g_ap_ssid;
std::string g_hostname;
esp_timer_handle_t g_retry_timer = nullptr;
esp_timer_handle_t g_fallback_timer = nullptr;
int g_retry_delay_s = 1;

void start_ap_locked() {
    if (g_ap_active) {
        return;
    }
    wifi_config_t ap = {};
    strncpy(reinterpret_cast<char*>(ap.ap.ssid), g_ap_ssid.c_str(), sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = static_cast<uint8_t>(std::min<size_t>(g_ap_ssid.size(), sizeof(ap.ap.ssid) - 1));
    strncpy(reinterpret_cast<char*>(ap.ap.password), AP_PASSWORD, sizeof(ap.ap.password) - 1);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
        return;
    }
    g_ap_active = true;
    ESP_LOGW(TAG, "Setup access point \"%s\" (password \"%s\") at http://192.168.4.1/",
             g_ap_ssid.c_str(), AP_PASSWORD);
}

void stop_ap_locked() {
    if (!g_ap_active || !g_sta_configured) {
        return;  // setup-only mode keeps its AP
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    g_ap_active = false;
    ESP_LOGI(TAG, "Setup access point stopped");
}

std::atomic<bool> g_scanning{false};

void retry_cb(void*) {
    if (g_scanning.load()) {
        return;  // scan() restarts the retry when it finishes
    }
    esp_wifi_connect();
}

void fallback_cb(void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_connected.load()) {
        ESP_LOGW(TAG, "Not connected to \"%s\" after 60 s; enabling setup access point", g_ssid.c_str());
        start_ap_locked();
    }
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_sta_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_connected.store(false);
        if (g_sta_configured && !g_scanning.load()) {
            auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
            ESP_LOGW(TAG, "Disconnected from \"%s\" (reason %d); retrying in %d s", g_ssid.c_str(),
                     ev != nullptr ? ev->reason : -1, g_retry_delay_s);
            esp_timer_stop(g_retry_timer);
            esp_timer_start_once(g_retry_timer, static_cast<uint64_t>(g_retry_delay_s) * 1000000ULL);
            g_retry_delay_s = std::min(g_retry_delay_s * 2, 30);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "Connected to \"%s\", IP " IPSTR, g_ssid.c_str(), IP2STR(&ev->ip_info.ip));
        g_retry_delay_s = 1;
        g_connected.store(true);
        std::lock_guard<std::mutex> lock(g_mutex);
        stop_ap_locked();
    }
}

}  // namespace

namespace wifi_manager {

esp_err_t init(const std::string& hostname, const std::string& ssid, const std::string& password) {
    g_hostname = hostname;
    g_ssid = ssid;
    g_sta_configured = !ssid.empty();
    g_ap_ssid = hostname.substr(0, 24) + "-setup";

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    g_sta = esp_netif_create_default_wifi_sta();
    g_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(g_sta, hostname.c_str());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    // Credentials live in our own settings, not in the Wi-Fi driver's NVS copy.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));

    esp_timer_create_args_t retry_args = {};
    retry_args.callback = retry_cb;
    retry_args.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &g_retry_timer));
    esp_timer_create_args_t fb_args = {};
    fb_args.callback = fallback_cb;
    fb_args.name = "wifi_ap_fb";
    ESP_ERROR_CHECK(esp_timer_create(&fb_args, &g_fallback_timer));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_sta_configured) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            wifi_config_t sta = {};
            strncpy(reinterpret_cast<char*>(sta.sta.ssid), ssid.c_str(), sizeof(sta.sta.ssid) - 1);
            strncpy(reinterpret_cast<char*>(sta.sta.password), password.c_str(),
                    sizeof(sta.sta.password) - 1);
            sta.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
            sta.sta.pmf_cfg.capable = true;
            sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
            sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
            err = esp_wifi_set_config(WIFI_IF_STA, &sta);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            // Setup mode: AP for the user, idle STA so the network scan works.
            start_ap_locked();
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    // Wi-Fi modem sleep stays enabled (the default): ESP-IDF requires it while Bluetooth runs.
    if (g_sta_configured) {
        esp_timer_start_once(g_fallback_timer, AP_FALLBACK_US);
    }
    return ESP_OK;
}

bool is_connected() {
    return g_connected.load();
}

WifiStatus status() {
    WifiStatus s;
    s.sta_configured = g_sta_configured;
    s.sta_connected = g_connected.load();
    s.ssid = g_ssid;
    s.hostname = g_hostname;
    s.ap_ssid = g_ap_ssid;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        s.ap_active = g_ap_active;
    }
    if (s.sta_connected && g_sta != nullptr) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_sta, &ip) == ESP_OK) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            s.ip = buf;
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s.rssi = ap.rssi;
        }
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        s.mac = buf;
    }
    return s;
}

std::vector<WifiNetwork> scan() {
    std::vector<WifiNetwork> out;
    // A station that is still trying to connect makes esp_wifi_scan_start() fail with
    // ESP_ERR_WIFI_STATE, which is exactly the situation the setup page is used in. Pause the
    // connection attempts for the duration of the scan.
    const bool pause_sta = g_sta_configured && !g_connected.load();
    g_scanning.store(true);
    if (pause_sta) {
        esp_timer_stop(g_retry_timer);
        esp_wifi_disconnect();
    }
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    g_scanning.store(false);
    if (pause_sta) {
        esp_timer_stop(g_retry_timer);
        esp_timer_start_once(g_retry_timer, 1000000);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        return out;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    count = std::min<uint16_t>(count, 30);
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            std::string ssid(reinterpret_cast<const char*>(records[i].ssid));
            if (ssid.empty()) {
                continue;
            }
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const WifiNetwork& n) { return n.ssid == ssid; });
            if (it != out.end()) {
                it->rssi = std::max(it->rssi, static_cast<int>(records[i].rssi));
                continue;
            }
            out.push_back({ssid, records[i].rssi, records[i].authmode != WIFI_AUTH_OPEN});
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    std::sort(out.begin(), out.end(), [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
    return out;
}

esp_err_t start_mdns(const std::string& hostname, const std::string& instance_name,
                     uint16_t sendspin_port) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    mdns_hostname_set(hostname.c_str());
    mdns_instance_name_set(instance_name.c_str());

    // Same TXT keys as sendspin-cpp's reference client: path and name.
    mdns_txt_item_t sendspin_txt[] = {
        {"path", "/sendspin"},
        {"name", instance_name.c_str()},
    };
    err = mdns_service_add(instance_name.c_str(), "_sendspin", "_tcp", sendspin_port, sendspin_txt,
                           sizeof(sendspin_txt) / sizeof(sendspin_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS _sendspin._tcp failed: %s", esp_err_to_name(err));
        return err;
    }
    err = mdns_service_add(instance_name.c_str(), "_http", "_tcp", 80, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _http._tcp failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "mDNS: %s.local, _sendspin._tcp port %u", hostname.c_str(), sendspin_port);
    return ESP_OK;
}

}  // namespace wifi_manager
