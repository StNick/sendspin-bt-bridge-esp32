// Sendspin Bluetooth Bridge for ESP32-S31.
//
// A firmware port of trudenboy/sendspin-bt-bridge: the device appears in Music Assistant as a
// Sendspin player and plays to one Bluetooth (A2DP) speaker. The Linux original runs one
// aiosendspin subprocess per speaker on PulseAudio/BlueZ; here sendspin-cpp decodes and
// synchronises the stream and an encoder worker supplies LDAC/AAC/SBC to Bluedroid.
//
// Threads:
//   main loop (this task)  Sendspin client loop and callbacks, idle machine, all Sendspin calls
//   sendspin tasks         protocol, sync/decode (writes PCM into the AudioPipe)
//   bt_encode              paced LDAC/AAC/SBC encoding (reads PCM from the AudioPipe)
//   bt_app                 Bluetooth state machine
//   pump                   drains the AudioPipe in real time while no speaker is consuming it
//   httpd                  web UI / REST API

#include <atomic>
#include <memory>
#include <string>

#include "app_config.h"
#include "app_state.h"
#include "audio_pipe.h"
#include "bt_manager.h"
#include "esp_app_desc.h"
#include "esp_avrc_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "repo_bt_diag.h"
#include "nvs_persistence.h"
#include "sendspin/client.h"
#include "sendspin/controller_role.h"
#include "sendspin/metadata_role.h"
#include "sendspin/player_role.h"
#include "web_server.h"
#include "wifi_manager.h"

using namespace sendspin;

namespace {

const char* TAG = "bridge";

// SBC fallback latency of the original bridge's latency model (_CODEC_FALLBACK_MS["sbc"]):
// the starting static delay for a speaker until it sends an AVDTP delay report or is calibrated.
constexpr uint16_t SBC_FALLBACK_DELAY_MS = 125;
constexpr size_t PIPE_DURATION_MS = 400;
constexpr size_t MIN_PSRAM_BYTES = 2 * 1024 * 1024;
constexpr int64_t US_PER_S = 1000000LL;
constexpr int64_t VOLUME_SAVE_DELAY_US = 5 * US_PER_S;

AudioPipe g_pipe;
std::unique_ptr<SendspinClient> g_client;
PlayerRole* g_player = nullptr;  // main loop
// The same pointer for the encoder and pump tasks. Set only after start()
// succeeds; notify_audio_played() is the only call made through it.
std::atomic<PlayerRole*> g_player_rt{nullptr};
ControllerRole* g_controller = nullptr;
MetadataRole* g_metadata = nullptr;
NvsPersistenceProvider g_persistence;

// ----- main-loop state -----
AppConfig g_cfg;
app_state::SendspinStatus g_ss;
bool g_stream_active = false;
bool g_paired = false;
bool g_bt_connected = false;
uint8_t g_volume = 50;
bool g_muted = false;
int64_t g_volume_dirty_since = 0;
int64_t g_idle_since = 0;
int64_t g_last_keepalive = 0;
bool g_power_save_applied = false;
bool g_idle_standby = false;
// Waking from idle standby: keep the player available while the speaker reconnects, so the
// playback that triggered the wake is not cancelled. Bounded by WAKE_TIMEOUT_US.
bool g_waking = false;
int64_t g_wake_deadline = 0;
constexpr int64_t WAKE_TIMEOUT_US = 60LL * 1000000LL;
bool g_available = true;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

const char* command_name(SendspinControllerCommand c) {
    switch (c) {
        case SendspinControllerCommand::PLAY: return "play";
        case SendspinControllerCommand::PAUSE: return "pause";
        case SendspinControllerCommand::STOP: return "stop";
        case SendspinControllerCommand::NEXT: return "next";
        case SendspinControllerCommand::PREVIOUS: return "previous";
        case SendspinControllerCommand::VOLUME: return "volume";
        case SendspinControllerCommand::MUTE: return "mute";
        case SendspinControllerCommand::REPEAT_OFF: return "repeat_off";
        case SendspinControllerCommand::REPEAT_ONE: return "repeat_one";
        case SendspinControllerCommand::REPEAT_ALL: return "repeat_all";
        case SendspinControllerCommand::SHUFFLE: return "shuffle";
        case SendspinControllerCommand::UNSHUFFLE: return "unshuffle";
        case SendspinControllerCommand::SWITCH: return "switch";
        case SendspinControllerCommand::SEEK: return "seek";
        case SendspinControllerCommand::SEEK_RELATIVE: return "seek_relative";
    }
    return "unknown";
}

/// The group's playback state as the client holds it now (reset when the server goes away,
/// which a cached on_group_update() flag would miss). Main loop only.
bool group_playing() {
    if (g_client == nullptr) {
        return false;
    }
    const auto& state = g_client->get_group_state().playback_state;
    return state.has_value() && *state == SendspinPlaybackState::PLAYING;
}

bool send_transport(SendspinControllerCommand cmd) {
    if (g_controller == nullptr) {
        return false;
    }
    ClientCommandControllerObject obj{};
    obj.command = cmd;
    return g_controller->send_command(obj);
}

void set_local_volume(uint8_t volume, bool push_to_speaker) {
    g_volume = volume;
    if (g_player != nullptr) {
        g_player->update_volume(volume);  // tells the server
    }
    if (push_to_speaker) {
        BtManager::instance().set_volume(g_volume, g_muted);
    }
    g_volume_dirty_since = esp_timer_get_time();
}

void set_local_muted(bool muted) {
    g_muted = muted;
    if (g_player != nullptr) {
        g_player->update_muted(muted);
    }
    BtManager::instance().set_volume(g_volume, g_muted);
}

void save_volume_if_due(int64_t now) {
    if (g_volume_dirty_since != 0 && now - g_volume_dirty_since >= VOLUME_SAVE_DELAY_US) {
        g_volume_dirty_since = 0;
        const AppConfig c = app_config::get();
        if (!c.has_last_volume || c.last_volume != g_volume) {
            const uint8_t volume = g_volume;
            app_config::update([volume](AppConfig& cfg) {
                cfg.has_last_volume = true;
                cfg.last_volume = volume;
            });
        }
    }
}

void set_available(bool available) {
    if (g_client == nullptr || available == g_available) {
        return;
    }
    g_available = available;
    g_client->set_available(available);
    ESP_LOGI(TAG, "Player %s in Music Assistant", available ? "available" : "unavailable");
}

// ---------------------------------------------------------------------------------------------
// Sendspin listeners
// ---------------------------------------------------------------------------------------------

struct PlayerListener : PlayerRoleListener {
    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
        return g_pipe.write(data, length, timeout_ms);
    }

    void on_stream_start() override {
        const auto& p = g_player->get_current_stream_params();
        if (p.sample_rate.value_or(0) != AudioPipe::sample_rate() ||
            p.channels.value_or(0) != AudioPipe::CHANNELS ||
            p.bit_depth.value_or(0) != AudioPipe::bits_per_sample()) {
            // Only the boot-selected PCM format is advertised to the server.
            ESP_LOGE(TAG, "Server chose an unadvertised format (%u Hz, %u ch, %u bit)",
                     static_cast<unsigned>(p.sample_rate.value_or(0)),
                     static_cast<unsigned>(p.channels.value_or(0)),
                     static_cast<unsigned>(p.bit_depth.value_or(0)));
        }
        g_stream_active = true;
        BtManager::instance().set_audio_active(true);
        ESP_LOGI(TAG, "Stream started");
    }

    void on_stream_end() override {
        // Fired only once the sync task is idle, so nothing is writing the pipe now.
        g_pipe.clear();
        g_stream_active = false;
        BtManager::instance().set_audio_active(false);
        g_idle_since = esp_timer_get_time();
        ESP_LOGI(TAG, "Stream ended");
    }

    void on_volume_changed(uint8_t volume) override {
        g_volume = volume;
        BtManager::instance().set_volume(g_volume, g_muted);
        g_volume_dirty_since = esp_timer_get_time();
    }

    void on_mute_changed(bool muted) override {
        g_muted = muted;
        BtManager::instance().set_volume(g_volume, g_muted);
    }

    void on_output_delay_changed(uint16_t delay_ms) override {
        ESP_LOGI(TAG, "Server set static delay to %u ms", delay_ms);
        if (app_config::get().delay_auto_pending) {
            // A deliberate value: stop auto-applying delay reports.
            app_config::update([](AppConfig& c) { c.delay_auto_pending = false; });
        }
    }
};

struct MetadataListener : MetadataRoleListener {
    void on_metadata(const ServerMetadataStateObject& md) override {
        g_ss.title = md.title.value_or("");
        g_ss.artist = md.artist.value_or("");
        g_ss.album = md.album.value_or("");
    }
    void on_metadata_clear() override {
        g_ss.title.clear();
        g_ss.artist.clear();
        g_ss.album.clear();
    }
};

struct ControllerListener : ControllerRoleListener {
    void on_controller_state(const ServerStateControllerObject& state) override {
        g_ss.supported_commands.clear();
        for (auto c : state.supported_commands) {
            g_ss.supported_commands.emplace_back(command_name(c));
        }
        g_ss.shuffle = state.shuffle;
        g_ss.repeat = state.repeat == SendspinRepeatMode::ONE   ? "one"
                      : state.repeat == SendspinRepeatMode::ALL ? "all"
                                                                : "off";
    }
    void on_controller_state_clear() override {
        g_ss.supported_commands.clear();
        g_ss.shuffle = false;
        g_ss.repeat.clear();
    }
};

struct ClientListener : SendspinClientListener {
    // Wi-Fi modem sleep is left on: ESP-IDF requires it while Bluetooth is enabled.
    void on_request_high_performance() override {}
    void on_release_high_performance() override {}
    void on_trust_changed(ConnectionTrust trust) override {
        g_paired = trust == ConnectionTrust::USER;
    }
    void on_pairing_started(const std::string&) override {
        g_ss.pairing_event = "Pairing started";
    }
    void on_pairing_succeeded(const std::string&) override {
        g_ss.pairing_event = "Paired with Music Assistant";
    }
    void on_pairing_failed(const std::string&, SendspinPairAbortReason) override {
        g_ss.pairing_event = "Pairing failed";
    }
    void on_display_pairing_code(const std::string& code, SendspinPairingCodeFormat) override {
        // DIGITS only (the only format advertised): show as "123 456".
        g_ss.pairing_code = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
    }
    void on_clear_pairing_code() override {
        g_ss.pairing_code.clear();
    }
    void on_open_pairing_window() override {
        g_ss.pairing_window_prompt = true;
    }
    void on_close_pairing_window() override {
        g_ss.pairing_window_prompt = false;
    }
};

struct NetworkProvider : SendspinNetworkProvider {
    bool is_network_ready() override {
        return wifi_manager::is_connected();
    }
};

PlayerListener g_player_listener;
MetadataListener g_metadata_listener;
ControllerListener g_controller_listener;
ClientListener g_client_listener;
NetworkProvider g_network;

// ---------------------------------------------------------------------------------------------
// Pump: keeps the Sendspin clock running while no speaker drains the pipe
// ---------------------------------------------------------------------------------------------

void pump_task(void*) {
    static uint8_t buf[512 * 6];  // maximum packed 24-bit stereo
    int64_t last = esp_timer_get_time();
    uint64_t frac_num = 0;  // accumulated frames * 1e6
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int64_t now = esp_timer_get_time();
        const int64_t elapsed = now - last;
        last = now;
        PlayerRole* player = g_player_rt.load();
        if (BtManager::instance().consuming() || g_pipe.buffered_bytes() == 0 || player == nullptr) {
            frac_num = 0;
            continue;
        }
        frac_num += static_cast<uint64_t>(elapsed) * AudioPipe::sample_rate();
        uint32_t due = static_cast<uint32_t>(frac_num / US_PER_S);
        frac_num %= US_PER_S;
        uint32_t drained = 0;
        while (due > 0) {
            const uint32_t chunk = due > 512 ? 512 : due;
            const size_t got = g_pipe.read(buf, chunk * AudioPipe::bytes_per_frame()) / AudioPipe::bytes_per_frame();
            if (got == 0) {
                break;
            }
            due -= static_cast<uint32_t>(got);
            drained += static_cast<uint32_t>(got);
        }
        if (drained > 0) {
            // These frames were due by now: they "finished" now.
            player->notify_audio_played(drained, now);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Idle machine (bridge/idle_machine.py), run on the main loop
// ---------------------------------------------------------------------------------------------

void run_idle_machine(int64_t now) {
    const bool active = g_stream_active || group_playing();
    if (active) {
        g_idle_since = now;
    }
    const int64_t idle_us = now - g_idle_since;

    // Leaving a mode undoes what it did.
    if (g_cfg.idle_mode != IdleMode::POWER_SAVE && g_power_save_applied) {
        BtManager::instance().set_power_save(false);
        g_power_save_applied = false;
    }
    if (g_cfg.idle_mode != IdleMode::AUTO_DISCONNECT && g_idle_standby) {
        BtManager::instance().exit_standby();
        g_idle_standby = false;
    }

    switch (g_cfg.idle_mode) {
        case IdleMode::POWER_SAVE:
            if (active && g_power_save_applied) {
                BtManager::instance().set_power_save(false);
                g_power_save_applied = false;
            } else if (!active && !g_power_save_applied && g_bt_connected &&
                       idle_us >= static_cast<int64_t>(g_cfg.power_save_delay_min) * 60 * US_PER_S) {
                ESP_LOGI(TAG, "Idle %u min: suspending the A2DP stream (power_save)", g_cfg.power_save_delay_min);
                BtManager::instance().set_power_save(true);
                g_power_save_applied = true;
            }
            g_ss.idle_state = g_power_save_applied ? "power saved" : (active ? "active" : "idle");
            break;
        case IdleMode::AUTO_DISCONNECT:
            if (active && g_idle_standby) {
                ESP_LOGI(TAG, "Playback requested: waking speaker from standby");
                BtManager::instance().exit_standby();
                g_idle_standby = false;
                g_waking = true;
                g_wake_deadline = now + WAKE_TIMEOUT_US;
            } else if (!active && !g_idle_standby && g_cfg.idle_disconnect_min > 0 && g_bt_connected &&
                       idle_us >= static_cast<int64_t>(g_cfg.idle_disconnect_min) * 60 * US_PER_S) {
                ESP_LOGI(TAG, "Idle %u min: releasing the speaker (auto_disconnect)", g_cfg.idle_disconnect_min);
                BtManager::instance().enter_standby();
                g_idle_standby = true;
            }
            g_ss.idle_state = g_idle_standby ? "standby" : (active ? "active" : "idle");
            break;
        case IdleMode::KEEP_ALIVE:
            if (!active && g_bt_connected &&
                now - g_last_keepalive >= static_cast<int64_t>(g_cfg.keepalive_interval_s) * US_PER_S) {
                g_last_keepalive = now;
                BtManager::instance().keepalive_burst(g_cfg.keepalive_method);
            }
            g_ss.idle_state = active ? "active" : "keep-alive";
            break;
        case IdleMode::DEFAULT:
        default:
            g_ss.idle_state = active ? "active" : "idle";
            break;
    }

    if (g_waking && now >= g_wake_deadline) {
        ESP_LOGW(TAG, "Speaker did not reconnect within %d s of the wake",
                 static_cast<int>(WAKE_TIMEOUT_US / US_PER_S));
        g_waking = false;
    }

    // As in the original: the player stays visible in MA while parked in idle standby (so MA
    // can wake it), and is withdrawn while the speaker is otherwise unreachable.
    set_available(g_bt_connected || g_idle_standby || g_waking);
}

// ---------------------------------------------------------------------------------------------
// Bluetooth -> main loop
// ---------------------------------------------------------------------------------------------

void on_speaker_passthrough(uint8_t key) {
    switch (key) {
        case ESP_AVRC_PT_CMD_PLAY:
            // Many speakers send PLAY for their single play/pause button.
            send_transport(group_playing() ? SendspinControllerCommand::PAUSE : SendspinControllerCommand::PLAY);
            break;
        case ESP_AVRC_PT_CMD_PAUSE:
            send_transport(SendspinControllerCommand::PAUSE);
            break;
        case ESP_AVRC_PT_CMD_STOP:
            if (!send_transport(SendspinControllerCommand::STOP)) {
                send_transport(SendspinControllerCommand::PAUSE);
            }
            break;
        case ESP_AVRC_PT_CMD_FORWARD:
            send_transport(SendspinControllerCommand::NEXT);
            break;
        case ESP_AVRC_PT_CMD_BACKWARD:
            send_transport(SendspinControllerCommand::PREVIOUS);
            break;
        case ESP_AVRC_PT_CMD_VOL_UP:
            set_local_volume(static_cast<uint8_t>(g_volume > 95 ? 100 : g_volume + 5), true);
            break;
        case ESP_AVRC_PT_CMD_VOL_DOWN:
            set_local_volume(static_cast<uint8_t>(g_volume < 5 ? 0 : g_volume - 5), true);
            break;
        case ESP_AVRC_PT_CMD_MUTE:
            set_local_muted(!g_muted);
            break;
        default:
            break;
    }
}

void on_speaker_delay_report(uint16_t delay_ms) {
    if (!app_config::get().delay_auto_pending || g_player == nullptr) {
        return;
    }
    const uint16_t ms = delay_ms > 5000 ? 5000 : delay_ms;
    ESP_LOGI(TAG, "Using the speaker's delay report (%u ms) as its static delay", ms);
    g_player->update_output_delay(ms);
    app_config::update([](AppConfig& c) { c.delay_auto_pending = false; });
}

// ---------------------------------------------------------------------------------------------
// Sendspin setup
// ---------------------------------------------------------------------------------------------

bool start_sendspin(const AppConfig& cfg) {
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram < MIN_PSRAM_BYTES) {
        g_ss.enabled = false;
        g_ss.disabled_reason = "PSRAM not found: sendspin-cpp needs about 1.3 MB of external RAM";
        ESP_LOGE(TAG, "%s (found %u bytes)", g_ss.disabled_reason.c_str(), static_cast<unsigned>(psram));
        return false;
    }

    SendspinClientConfig config;
    config.name = cfg.name;
    config.product_name = "Sendspin Bluetooth Bridge (ESP32-S31)";
    config.software_version = esp_app_get_description()->version;
    // The web UI is this device's display for dynamic pairing codes.
    config.pairing_code_out_channels = {SendspinPairingCodeChannel::DISPLAY};
    config.pairing_code_formats = {SendspinPairingCodeFormat::DIGITS};

    g_client = std::make_unique<SendspinClient>(std::move(config));

    PlayerRoleConfig player_config;
    // Advertise the selected format, FLAC first to reduce shared-radio traffic.
    player_config.audio_formats = {
        {SendspinCodecFormat::FLAC, AudioPipe::CHANNELS, AudioPipe::sample_rate(), AudioPipe::bits_per_sample()},
        {SendspinCodecFormat::PCM, AudioPipe::CHANNELS, AudioPipe::sample_rate(), AudioPipe::bits_per_sample()},
    };
    player_config.initial_output_delay_ms = SBC_FALLBACK_DELAY_MS;
    player_config.min_buffer_ms = cfg.min_buffer_ms;
    if (cfg.lead_time_ms > 0) {
        player_config.required_lead_time_ms = cfg.lead_time_ms;
    }
    g_player = &g_client->add_player(std::move(player_config));
    g_controller = &g_client->add_controller();
    g_metadata = &g_client->add_metadata();

    g_player->set_listener(&g_player_listener);
    g_controller->set_listener(&g_controller_listener);
    g_metadata->set_listener(&g_metadata_listener);
    g_client->set_listener(&g_client_listener);
    g_client->set_network_provider(&g_network);
    g_client->set_persistence_provider(&g_persistence);
    // SENDSPIN_PAIRING off (the original's default) means unpaired servers may play.
    g_client->set_unpaired_access_enabled(!cfg.require_pairing);

    if (!g_client->start()) {
        g_ss.enabled = false;
        g_ss.disabled_reason = "Sendspin client failed to start (see logs)";
        ESP_LOGE(TAG, "%s", g_ss.disabled_reason.c_str());
        g_player = nullptr;
        g_controller = nullptr;
        g_metadata = nullptr;
        g_client.reset();
        return false;
    }

    g_player_rt.store(g_player);
    g_player->set_output_delay_adjustable(true);
    if (cfg.has_last_volume) {
        g_volume = cfg.last_volume;
        g_player->update_volume(g_volume);
    } else {
        g_volume = g_player->get_volume();
    }
    BtManager::instance().set_volume(g_volume, g_muted);

    if (!cfg.server_url.empty()) {
        ESP_LOGI(TAG, "Dialling Sendspin server %s", cfg.server_url.c_str());
        g_client->connect_to(cfg.server_url);
    }

    g_available = true;  // the library's initial state
    set_available(false);  // until the speaker is connected
    g_ss.enabled = true;
    g_ss.client_id = g_client->client_id();
    ESP_LOGI(TAG, "Sendspin player \"%s\" started, client_id %s", cfg.name.c_str(), g_ss.client_id.c_str());
    return true;
}

void refresh_status() {
    if (g_client != nullptr) {
        g_ss.connected = g_client->is_connected();
        g_ss.paired = g_ss.connected && g_paired;
        g_ss.time_synced = g_client->is_time_synced();
        g_ss.unpaired_access = g_client->is_unpaired_access_enabled();
        auto info = g_client->get_server_information();
        g_ss.server_name = info ? info->name : "";
        g_ss.server_id = info ? info->server_id : "";
        const GroupUpdateObject& group = g_client->get_group_state();
        g_ss.group_name = group.group_name.value_or("");
        if (group.playback_state.has_value()) {
            g_ss.playback_state = *group.playback_state == SendspinPlaybackState::PLAYING ? "playing" : "stopped";
        } else {
            g_ss.playback_state.clear();
        }
    }
    if (g_player != nullptr) {
        g_ss.volume = g_volume;
        g_ss.muted = g_muted;
        g_ss.output_delay_ms = g_player->get_output_delay_ms();
        g_ss.stream_active = g_stream_active;
        const auto& p = g_player->get_current_stream_params();
        g_ss.sample_rate = p.sample_rate.value_or(0);
        g_ss.bit_depth = p.bit_depth.value_or(0);
        if (g_stream_active && p.codec.has_value()) {
            g_ss.stream_codec = *p.codec == SendspinCodecFormat::FLAC ? "FLAC"
                                : *p.codec == SendspinCodecFormat::PCM ? "PCM"
                                                                       : "other";
        } else {
            g_ss.stream_codec.clear();
        }
    }
    if (g_metadata != nullptr) {
        g_ss.progress_ms = g_metadata->get_track_progress_ms();
        g_ss.duration_ms = g_metadata->get_track_duration_ms();
    }
    app_state::set_sendspin(g_ss);
}

// ---------------------------------------------------------------------------------------------
// Web actions (run on the main loop via app_state::post)
// ---------------------------------------------------------------------------------------------

WebActions make_web_actions() {
    WebActions a;
    a.transport = [](const std::string& command) -> bool {
        SendspinControllerCommand cmd;
        if (command == "play") {
            cmd = SendspinControllerCommand::PLAY;
        } else if (command == "pause") {
            cmd = SendspinControllerCommand::PAUSE;
        } else if (command == "stop") {
            cmd = SendspinControllerCommand::STOP;
        } else if (command == "next") {
            cmd = SendspinControllerCommand::NEXT;
        } else if (command == "previous") {
            cmd = SendspinControllerCommand::PREVIOUS;
        } else {
            return false;
        }
        // send_command() is callable from any thread and reports whether it was queued.
        return send_transport(cmd);
    };
    a.set_volume = [](uint8_t volume) {
        app_state::post([volume] { set_local_volume(volume, true); });
    };
    a.set_muted = [](bool muted) {
        app_state::post([muted] { set_local_muted(muted); });
    };
    a.set_delay = [](uint16_t delay_ms) {
        app_state::post([delay_ms] {
            if (g_player != nullptr) {
                g_player->update_output_delay(delay_ms);
            }
            app_config::update([](AppConfig& c) { c.delay_auto_pending = false; });
        });
    };
    a.pairing_confirm = [] {
        app_state::post([] {
            if (g_client != nullptr) {
                g_client->confirm_pairing_window();
            }
            g_ss.pairing_window_prompt = false;
        });
    };
    a.pairing_cancel = [] {
        app_state::post([] {
            if (g_client != nullptr) {
                g_client->cancel_pairing_window();
            }
            g_ss.pairing_window_prompt = false;
        });
    };
    a.speaker_selected = [] {
        app_state::post([] {
            // New speaker: back to the SBC fallback until it reports or is calibrated.
            if (g_player != nullptr) {
                g_player->update_output_delay(SBC_FALLBACK_DELAY_MS);
            }
            g_power_save_applied = false;
            g_idle_standby = false;
        });
    };
    a.settings_changed = [](const AppConfig& before, const AppConfig& after) {
        app_state::post([before, after] {
            g_cfg = after;
            if (before.ldac_quality != after.ldac_quality) {
                BtManager::instance().set_ldac_quality(after.ldac_quality);
            }
            if (before.require_pairing != after.require_pairing && g_client != nullptr) {
                g_client->set_unpaired_access_enabled(!after.require_pairing);
            }
            if (before.bt_pin != after.bt_pin) {
                BtManager::instance().set_pin(after.bt_pin);
            }
            if (before.bt_check_interval_s != after.bt_check_interval_s ||
                before.bt_max_reconnect_fails != after.bt_max_reconnect_fails) {
                BtManager::instance().set_policy(after.bt_check_interval_s, after.bt_max_reconnect_fails);
            }
            if (before.idle_mode != after.idle_mode) {
                g_idle_since = esp_timer_get_time();
                g_last_keepalive = g_idle_since;
            }
        });
    };
    return a;
}

}  // namespace

extern "C" void app_main(void) {
    ESP_ERROR_CHECK(app_config::init());
    log_buffer::init(16 * 1024);
    g_cfg = app_config::get();

    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "Sendspin Bluetooth Bridge %s (ESP-IDF %s), player \"%s\"", app->version, app->idf_ver,
             g_cfg.name.c_str());
    ESP_LOGI(TAG, "PSRAM: %u bytes", static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));

    AudioPipe::configure_format(g_cfg.audio_sample_rate, g_cfg.audio_bit_depth);
    const size_t pipe_bytes = AudioPipe::sample_rate() * PIPE_DURATION_MS / 1000 * AudioPipe::bytes_per_frame();
    if (!g_pipe.init(pipe_bytes)) {
        ESP_LOGE(TAG, "Audio pipe allocation failed");
    }
    if (g_cfg.has_last_volume) {
        g_volume = g_cfg.last_volume;
    }

    const std::string hostname = app_config::hostname_from_name(g_cfg.name);

    BtManager::Callbacks bt_callbacks;
    bt_callbacks.frames_played = [](uint32_t frames, int64_t finish_us) {
        PlayerRole* player = g_player_rt.load(std::memory_order_acquire);
        if (player != nullptr) {
            player->notify_audio_played(frames, finish_us);
        }
    };
    bt_callbacks.speaker_volume = [](uint8_t volume) {
        app_state::post([volume] { set_local_volume(volume, false); });
    };
    bt_callbacks.passthrough = [](uint8_t key) {
        app_state::post([key] { on_speaker_passthrough(key); });
    };
    bt_callbacks.delay_report = [](uint16_t delay_ms) {
        app_state::post([delay_ms] { on_speaker_delay_report(delay_ms); });
    };
    bt_callbacks.connection = [](bool connected) {
        app_state::post([connected] {
            g_bt_connected = connected;
            if (connected) {
                const int64_t now = esp_timer_get_time();
                g_idle_since = now;
                g_last_keepalive = now;
                g_power_save_applied = false;
                g_idle_standby = false;  // the speaker may have reconnected by itself
                g_waking = false;
                BtManager::instance().set_power_save(false);
            }
        });
    };
    esp_err_t err = BtManager::instance().init(g_cfg, &g_pipe, std::move(bt_callbacks));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth init failed: %s", esp_err_to_name(err));
    }

    err = wifi_manager::init(hostname, g_cfg.wifi_ssid, g_cfg.wifi_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    }
    err = wifi_manager::start_mdns(hostname, g_cfg.name, SendspinClientConfig::DEFAULT_SERVER_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS failed: %s", esp_err_to_name(err));
    }

    start_sendspin(g_cfg);
    app_state::set_sendspin(g_ss);

    web_server::start(make_web_actions());

    xTaskCreate(pump_task, "pump", 3072, nullptr, 5, nullptr);

    g_idle_since = esp_timer_get_time();
    g_last_keepalive = g_idle_since;
    int64_t next_status = 0;
    int64_t next_transport_log = 0;
    uint32_t last_transport_faults = 0;
    while (true) {
        app_state::run_pending();
        if (g_client != nullptr) {
            g_client->loop();
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_transport_log) {
            next_transport_log = now + 10000000;
            repo_bt_diag_t d{};
            repo_bt_diag_get(&d);
            const uint32_t faults = d.source_rejected + d.path_dropped + d.media_write_failures;
            if (faults > last_transport_faults) {
                ESP_LOGW(TAG, "BT transport: queue=%lu peak=%lu rejected=%lu path_drop=%lu write_fail=%lu drain_gap=%lu us controller_gap=%lu us",
                    (unsigned long)d.source_depth, (unsigned long)d.source_high_water,
                    (unsigned long)d.source_rejected, (unsigned long)d.path_dropped,
                    (unsigned long)d.media_write_failures, (unsigned long)d.source_gap_us_max,
                    (unsigned long)d.controller_gap_us_max);
            }
            last_transport_faults = faults;
        }
        run_idle_machine(now);
        save_volume_if_due(now);
        if (now >= next_status) {
            next_status = now + 250000;
            refresh_status();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
