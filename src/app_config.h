// Persistent bridge settings (NVS namespace "bridge").
//
// Mirrors the per-device settings of sendspin-bt-bridge's BLUETOOTH_DEVICES entry and the
// bridge-level options that still make sense on a single-speaker microcontroller.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"

/// Per-device idle behaviour (sendspin-bt-bridge `idle_mode`).
enum class IdleMode : uint8_t {
    DEFAULT = 0,          // no action: keep the A2DP link and stream open
    POWER_SAVE = 1,       // suspend the A2DP stream after power_save_delay_min idle minutes
    AUTO_DISCONNECT = 2,  // drop the Bluetooth link after idle_disconnect_min idle minutes
    KEEP_ALIVE = 3,       // send a short inaudible burst every keepalive_interval_s seconds
};

/// Payload for keep-alive bursts (sendspin-bt-bridge `keep_alive_method`).
enum class KeepAliveMethod : uint8_t {
    INFRASOUND = 0,  // 1 s of 2 Hz stereo tone at amplitude 100 (about -50 dBFS)
    SILENCE = 1,     // 1 s of zero PCM
    NONE = 2,        // no burst
};

enum class LdacQuality : uint8_t { HIGH = 0, STANDARD = 1, CONNECTION = 2 };
const char* ldac_quality_name(LdacQuality quality);
bool ldac_quality_from_name(const std::string& name, LdacQuality* out);

const char* idle_mode_name(IdleMode mode);
bool idle_mode_from_name(const std::string& name, IdleMode* out);
const char* keepalive_method_name(KeepAliveMethod method);
bool keepalive_method_from_name(const std::string& name, KeepAliveMethod* out);

struct AppConfig {
    // Network
    std::string wifi_ssid;
    std::string wifi_password;

    // Player name shown in Music Assistant; also the Bluetooth and mDNS host name base.
    std::string name;

    // Target speaker
    bool has_speaker{false};
    std::array<uint8_t, 6> speaker_bda{};
    std::string speaker_name;
    std::string bt_pin{"0000"};  // legacy (pre-2.1) PIN pairing only
    LdacQuality ldac_quality{LdacQuality::STANDARD};
    uint32_t audio_sample_rate{44100};
    uint8_t audio_bit_depth{16};

    // Idle behaviour
    IdleMode idle_mode{IdleMode::DEFAULT};
    uint16_t power_save_delay_min{1};
    uint16_t idle_disconnect_min{0};  // 0 = disabled
    uint16_t keepalive_interval_s{30};  // minimum 30, as in the original bridge
    KeepAliveMethod keepalive_method{KeepAliveMethod::INFRASOUND};

    // Bluetooth reconnect policy (BT_CHECK_INTERVAL / BT_MAX_RECONNECT_FAILS)
    uint16_t bt_check_interval_s{10};
    uint16_t bt_max_reconnect_fails{0};  // 0 = unlimited

    // Sendspin
    std::string server_url;      // optional ws://host:port/path to dial; empty = wait for discovery
    bool require_pairing{false};  // SENDSPIN_PAIRING: off = unpaired servers may play
    uint16_t min_buffer_ms{500};
    uint16_t lead_time_ms{250};
    // One-shot marker (static_delay_source == "auto_pending"): the first AVDTP delay report
    // from a newly selected speaker becomes its static delay.
    bool delay_auto_pending{true};

    // Last player volume, restored after a reboot (LAST_VOLUMES)
    bool has_last_volume{false};
    uint8_t last_volume{50};

    // Optional web UI password (HTTP Basic auth, user "admin"); empty = no auth
    std::string web_password;
};

namespace app_config {

/// Opens NVS (erasing it if its layout is from an incompatible version) and loads settings.
esp_err_t init();

/// Returns a copy of the current settings. Thread-safe.
AppConfig get();

/// Replaces the settings and writes them to NVS. Thread-safe.
esp_err_t set(const AppConfig& config);

/// Read-modify-write under the settings lock, so concurrent partial updates from different
/// tasks (web handlers, the main loop) cannot overwrite each other. Thread-safe.
esp_err_t update(const std::function<void(AppConfig&)>& mutate);

/// Erases the whole NVS partition (settings, Sendspin identity, Bluetooth bonds).
esp_err_t factory_reset();

/// Lowercase hostname derived from a display name ("Living Room" -> "living-room").
std::string hostname_from_name(const std::string& name);

std::string bda_to_string(const std::array<uint8_t, 6>& bda);
bool bda_from_string(const std::string& text, std::array<uint8_t, 6>* out);

}  // namespace app_config
