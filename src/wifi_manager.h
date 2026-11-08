// Wi-Fi station with a setup access point, plus mDNS advertisement.
//
// - No saved network: the device starts an access point "<hostname>-setup" (WPA2, password
//   "sendspin") and serves the web UI at http://192.168.4.1/ so Wi-Fi can be entered.
// - Saved network: station mode with retries; if it has not connected after 60 s the setup
//   access point is started alongside so the settings can be fixed. It stops again on connect.
// - mDNS: <hostname>.local, _sendspin._tcp (port 8928, path /sendspin) so Music Assistant
//   discovers the player, and _http._tcp for the web UI.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

struct WifiStatus {
    bool sta_configured{false};
    bool sta_connected{false};
    std::string ssid;
    std::string ip;
    int rssi{0};
    bool ap_active{false};
    std::string ap_ssid;
    std::string hostname;
    std::string mac;
};

struct WifiNetwork {
    std::string ssid;
    int rssi{0};
    bool secure{false};
};

namespace wifi_manager {

esp_err_t init(const std::string& hostname, const std::string& ssid, const std::string& password);

/// True once the station has an IPv4 address. Cheap; safe from any task.
bool is_connected();

WifiStatus status();

/// Blocking scan (a few seconds). Call from a web handler, not from the main loop.
std::vector<WifiNetwork> scan();

/// Starts advertising _sendspin._tcp with the player name as instance name.
esp_err_t start_mdns(const std::string& hostname, const std::string& instance_name,
                     uint16_t sendspin_port);

}  // namespace wifi_manager
