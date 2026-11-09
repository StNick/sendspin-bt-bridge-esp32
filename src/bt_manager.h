// Bluetooth Classic A2DP source for one speaker: discovery, pairing, connection with the
// original bridge's reconnect backoff, media start/suspend, AVRCP absolute volume and speaker
// button forwarding, plus an AAC/SBC encoder worker that drains the AudioPipe.
//
// All Bluedroid callbacks are copied onto a queue and handled on one "bt_app" task, so the
// state below is only touched from that task. Public methods are thread-safe and asynchronous.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "app_config.h"
#include "bt_encoder.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class AudioPipe;

struct BtScanResult {
    std::array<uint8_t, 6> bda{};
    std::string name;
    uint32_t cod{0};
    int rssi{-127};
    bool audio{false};   // Class of Device says audio/video or "rendering" service
    bool bonded{false};
};

struct BtStatus {
    bool ready{false};
    std::string local_mac;
    std::string state;  // no_speaker, disconnected, connecting, connected, streaming,
                        // disconnecting, standby, gave_up
    bool has_target{false};
    std::string target_mac;
    std::string target_name;
    bool connected{false};
    bool streaming{false};     // A2DP media started
    bool power_saved{false};   // media suspended by power_save idle mode
    bool standby{false};       // link released by auto_disconnect idle mode or by the user
    bool gave_up{false};       // reconnect stopped after BT_MAX_RECONNECT_FAILS
    int reconnect_attempt{0};
    int next_attempt_in_s{-1};
    bool scanning{false};
    std::vector<BtScanResult> scan_results;
    std::vector<std::string> bonded;
    bool abs_volume{false};
    bool rssi_valid{false};
    int rssi_dbm{0};           // HCI Read RSSI: dB relative to the golden range (0 = ideal)
    int delay_report_ms{-1};   // AVDTP sink delay report, -1 if none received
    bool mono_sink{false};
    std::string codec;
    uint32_t bitrate_bps{0};
    BtEncoder::Metrics encoder_metrics{};
    std::string last_event;
};

class BtManager {
public:
    struct Callbacks {
        // Encoder worker: real (non-filler) frames accepted by A2DP and the estimated client
        // time of their last sample. Must be fast and non-blocking.
        std::function<void(uint32_t frames, int64_t finish_us)> frames_played;
        // bt_app task: the speaker changed its own volume (AVRCP absolute volume), 0-100.
        std::function<void(uint8_t volume)> speaker_volume;
        // bt_app task: a button on the speaker sent an AVRCP passthrough key (ESP_AVRC_PT_CMD_*).
        std::function<void(uint8_t key)> passthrough;
        // bt_app task: AVDTP delay report from the speaker, in milliseconds.
        std::function<void(uint16_t delay_ms)> delay_report;
        // bt_app task: A2DP link to the target speaker came up or went down.
        std::function<void(bool connected)> connection;
    };

    static BtManager& instance();

    esp_err_t init(const AppConfig& config, AudioPipe* pipe, Callbacks callbacks);

    void set_target(const std::array<uint8_t, 6>& bda, const std::string& name);
    void clear_target(bool remove_bond);
    void start_scan();
    void reconnect_now();
    void enter_standby();
    void exit_standby();
    void set_power_save(bool suspended);
    void set_volume(uint8_t volume, bool muted);
    void keepalive_burst(KeepAliveMethod method);
    void set_policy(uint16_t check_interval_s, uint16_t max_reconnect_fails);
    void set_pin(const std::string& pin);
    void set_ldac_quality(LdacQuality quality);

    BtStatus status();
    bool is_connected() const {
        return connected_flag_.load(std::memory_order_relaxed);
    }
    /// True while the A2DP encoder is pulling PCM from the pipe (callback seen in the last 150 ms).
    bool consuming() const;

private:
    BtManager() = default;

    struct Msg;
    enum class Conn : uint8_t { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING };
    enum class Media : uint8_t { IDLE, CHECKING, STARTING, STARTED, SUSPENDING };

    static void task_entry(void* arg);
    void task_loop();
    void post(const Msg& msg, TickType_t wait);
    void handle(const Msg& msg);
    void handle_gap(const Msg& msg);
    void handle_a2d(const Msg& msg);
    void handle_ct(const Msg& msg);
    void handle_tg(const Msg& msg);
    void handle_cmd(const Msg& msg);
    void tick();
    void start_connect();
    void schedule_retry_after_failure();
    void refresh_bonded();
    void apply_volume();
    void apply_tg_filter();
    void publish(const char* event = nullptr);
    bool want_media() const;
    int64_t backoff_us(int attempt) const;

    void fill_idle(uint8_t* dst, size_t bytes, int channels);
    void apply_gain(int16_t* samples, size_t count);
    uint32_t read_pcm(uint8_t* data, size_t len);

    // Bluedroid callbacks (run on the BTC task): copy and queue.
    static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static void ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param);
    static void tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param);

    AudioPipe* pipe_{nullptr};
    Callbacks callbacks_;
    QueueHandle_t queue_{nullptr};
    BtEncoder encoder_;
    esp_a2d_conn_hdl_t audio_connection_{0};
    uint16_t audio_mtu_{0};
    uint8_t registered_seps_{0};

    // bt_app task state
    bool has_target_{false};
    std::array<uint8_t, 6> target_{};
    std::string target_name_;
    std::array<uint8_t, 6> peer_{};
    std::array<uint8_t, 6> link_bda_{};  // device of the link being opened or open
    bool pending_disconnect_{false};     // standby requested while the link was opening
    Conn conn_{Conn::DISCONNECTED};
    Media media_{Media::IDLE};
    int64_t connect_started_us_{0};
    int64_t next_attempt_us_{0};
    int64_t next_media_us_{0};
    int64_t media_cmd_us_{0};
    int64_t next_rssi_us_{0};
    int attempts_{0};
    bool gave_up_{false};
    bool standby_{false};
    bool connect_asap_{false};
    bool power_saved_{false};
    bool scanning_{false};
    bool rc_connected_{false};
    bool abs_volume_{false};
    int last_sent_abs_{-1};
    int64_t last_abs_cmd_us_{0};
    uint8_t volume_{50};
    bool muted_{false};
    bool rssi_valid_{false};
    int rssi_{0};
    int delay_report_ms_{-1};
    bool tg_filter_set_{false};
    uint16_t check_interval_s_{10};
    uint16_t max_fails_{0};
    std::vector<BtScanResult> scan_results_;
    std::vector<std::array<uint8_t, 6>> bonded_;

    // Shared with the A2DP source task / other tasks
    std::atomic<bool> connected_flag_{false};
    std::atomic<bool> mono_{false};
    std::atomic<uint32_t> gain_q16_{65536};
    std::atomic<int64_t> last_data_cb_us_{0};
    std::atomic<uint32_t> keepalive_frames_{0};
    std::atomic<uint8_t> keepalive_method_{0};
    uint32_t infrasound_phase_{0};  // A2DP source task only

    std::mutex pin_mutex_;
    std::string pin_{"0000"};

    std::mutex status_mutex_;
    BtStatus status_;
    std::string local_mac_;
    std::string last_event_;
};
