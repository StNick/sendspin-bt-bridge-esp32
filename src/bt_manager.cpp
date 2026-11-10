#include "bt_manager.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "audio_pipe.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "repo_ldac_transport.h"

namespace {

const char* TAG = "bt";

constexpr int64_t US_PER_S = 1000000LL;
constexpr int64_t MAX_RECONNECT_DELAY_US = 300LL * US_PER_S;  // reconnect_policy.MAX_RECONNECT_DELAY_S
constexpr int64_t CONNECT_TIMEOUT_US = 30LL * US_PER_S;
constexpr int64_t MEDIA_CMD_TIMEOUT_US = 5LL * US_PER_S;
constexpr int64_t RSSI_POLL_US = 5LL * US_PER_S;               // RSSI_BADGE polls every 5 s
constexpr int64_t CONSUMING_WINDOW_US = 150000;                // > 2x Bluedroid's 30 ms media tick
constexpr float INFRASOUND_AMPLITUDE = 100.0f;                  // _INFRASOUND_AMP
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr size_t MAX_SCAN_RESULTS = 40;
constexpr int64_t ABS_VOLUME_ECHO_US = 1500000;  // window for our own volume command's echo
constexpr UBaseType_t EVENT_QUEUE_DEPTH = 48;
constexpr uint8_t INQUIRY_LEN = 10;  // x 1.28 s

// AVRCP transaction labels
constexpr uint8_t TL_GET_CAPS = 0;
constexpr uint8_t TL_RN_VOLUME = 1;
constexpr uint8_t TL_SET_VOLUME = 2;

enum class Kind : uint8_t { GAP, A2D, CT, TG, CMD };

enum class Cmd : uint8_t {
    SET_TARGET,
    CLEAR_TARGET,
    SCAN,
    RECONNECT,
    STANDBY,
    WAKE,
    POWER_SAVE,
    VOLUME,
    POLICY,
    LDAC_QUALITY,
};

struct DiscResult {
    esp_bd_addr_t bda;
    char name[64];
    uint32_t cod;
    int8_t rssi;
};

struct AuthResult {
    esp_bd_addr_t bda;
    esp_bt_status_t stat;
    char name[64];
};

struct RssiResult {
    esp_bd_addr_t bda;
    esp_bt_status_t stat;
    int8_t rssi;
};

struct CmdData {
    Cmd type;
    esp_bd_addr_t bda;
    char name[64];
    int32_t a;
    int32_t b;
};

bool bda_equal(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, ESP_BD_ADDR_LEN) == 0;
}

std::array<uint8_t, 6> to_array(const uint8_t* bda) {
    std::array<uint8_t, 6> out{};
    memcpy(out.data(), bda, out.size());
    return out;
}

void copy_name(char* dst, size_t dst_len, const uint8_t* src, size_t src_len) {
    size_t n = std::min(src_len, dst_len - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

uint32_t gain_for(uint8_t volume, bool muted) {
    if (muted || volume == 0) {
        return 0;
    }
    if (volume >= 100) {
        return 65536;
    }
    // Quadratic curve (vol/100)^2, as sendspin-cpp's reference audio sink uses.
    return static_cast<uint32_t>((static_cast<uint64_t>(volume) * volume * 65536ULL) / 10000ULL);
}

uint8_t to_avrcp(uint8_t volume) {
    return static_cast<uint8_t>((static_cast<uint32_t>(volume) * 127U + 50U) / 100U);
}

uint8_t from_avrcp(uint8_t abs) {
    return static_cast<uint8_t>((static_cast<uint32_t>(abs & 0x7F) * 100U + 63U) / 127U);
}

// Scratch for the mono down-mix; only the A2DP source task touches it.
constexpr size_t SCRATCH_FRAMES = 256;
uint8_t g_scratch[SCRATCH_FRAMES * 6];

}  // namespace

struct BtManager::Msg {
    Kind kind;
    uint16_t event;
    union {
        esp_a2d_cb_param_t a2d;
        esp_avrc_ct_cb_param_t ct;
        esp_avrc_tg_cb_param_t tg;
        DiscResult disc;
        AuthResult auth;
        RssiResult rssi;
        esp_bt_gap_discovery_state_t disc_state;
        CmdData cmd;
    } u;
};

BtManager& BtManager::instance() {
    static BtManager manager;
    return manager;
}

// ---------------------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------------------

esp_err_t BtManager::init(const AppConfig& config, AudioPipe* pipe, Callbacks callbacks) {
    pipe_ = pipe;
    callbacks_ = std::move(callbacks);
    encoder_.set_ldac_quality(config.ldac_quality);
    check_interval_s_ = config.bt_check_interval_s;
    max_fails_ = config.bt_max_reconnect_fails;
    has_target_ = config.has_speaker;
    target_ = config.speaker_bda;
    target_name_ = config.speaker_name;
    {
        std::lock_guard<std::mutex> lock(pin_mutex_);
        pin_ = config.bt_pin;
    }
    if (config.has_last_volume) {
        volume_ = config.last_volume;
    }
    gain_q16_.store(gain_for(volume_, muted_));

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        local_mac_ = app_config::bda_to_string(to_array(mac));
    }

    queue_ = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(Msg));
    if (queue_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        return err;
    }

    // Secure Simple Pairing with no I/O: "Just Works", which is what speakers expect.
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    // Legacy pairing: variable PIN, answered from settings in ESP_BT_GAP_PIN_REQ_EVT.
    esp_bt_pin_code_t pin_code = {0};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);

    if (xTaskCreate(task_entry, "bt_app", 6144, this, 5, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name(config.name.c_str());

    // AVRCP before A2DP, as in ESP-IDF's examples.
    esp_avrc_ct_register_callback(ct_cb);
    err = esp_avrc_ct_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP CT init failed: %s", esp_err_to_name(err));
    }
    esp_avrc_tg_register_callback(tg_cb);
    err = esp_avrc_tg_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP TG init failed: %s", esp_err_to_name(err));
    }
    // The passthrough filter is set from the bt_app task (tick / ESP_AVRC_TG_PROF_STATE_EVT).

    esp_a2d_register_callback(a2d_cb);
    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(err));
        return err;
    }
    const bool ldac_enabled = config.ldac_quality != LdacQuality::OFF;
    required_seps_ = ldac_enabled ? 0b111 : 0b011;
    err = BtEncoder::register_endpoints(ldac_enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP codec registration failed: %s", esp_err_to_name(err));
        return err;
    }

    // Connectable so a bonded speaker can reconnect to us; not discoverable (we are a source).
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    ESP_LOGI(TAG, "Bluetooth ready, local address %s", local_mac_.c_str());
    return ESP_OK;
}

void BtManager::apply_tg_filter() {
    if (tg_filter_set_) {
        return;
    }
    esp_avrc_psth_bit_mask_t allowed = {};
    if (esp_avrc_tg_get_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_ALLOWED_CMD, &allowed) != ESP_OK) {
        return;
    }
    const esp_avrc_pt_cmd_t wanted[] = {
        ESP_AVRC_PT_CMD_PLAY,    ESP_AVRC_PT_CMD_PAUSE,    ESP_AVRC_PT_CMD_STOP,
        ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_VOL_UP,
        ESP_AVRC_PT_CMD_VOL_DOWN, ESP_AVRC_PT_CMD_MUTE,
    };
    esp_avrc_psth_bit_mask_t supported = {};
    for (esp_avrc_pt_cmd_t cmd : wanted) {
        if (esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &allowed, cmd)) {
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &supported, cmd);
        }
    }
    if (esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &supported) == ESP_OK) {
        tg_filter_set_ = true;
    }
}

// ---------------------------------------------------------------------------------------------
// Bluedroid callbacks (BTC task): copy what we need and queue it
// ---------------------------------------------------------------------------------------------

void BtManager::gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    BtManager& self = instance();
    Msg msg{};
    msg.kind = Kind::GAP;
    msg.event = static_cast<uint16_t>(event);

    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            DiscResult& r = msg.u.disc;
            memcpy(r.bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
            r.rssi = -127;
            uint8_t* eir = nullptr;
            for (int i = 0; i < param->disc_res.num_prop; ++i) {
                esp_bt_gap_dev_prop_t* p = param->disc_res.prop + i;
                switch (p->type) {
                    case ESP_BT_GAP_DEV_PROP_COD:
                        r.cod = *static_cast<uint32_t*>(p->val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_RSSI:
                        r.rssi = *static_cast<int8_t*>(p->val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_BDNAME:
                        copy_name(r.name, sizeof(r.name), static_cast<uint8_t*>(p->val),
                                  static_cast<size_t>(p->len));
                        break;
                    case ESP_BT_GAP_DEV_PROP_EIR:
                        eir = static_cast<uint8_t*>(p->val);
                        break;
                    default:
                        break;
                }
            }
            if (r.name[0] == '\0' && eir != nullptr) {
                uint8_t len = 0;
                uint8_t* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (name == nullptr) {
                    name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                }
                if (name != nullptr) {
                    copy_name(r.name, sizeof(r.name), name, len);
                }
            }
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            msg.u.disc_state = param->disc_st_chg.state;
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            memcpy(msg.u.auth.bda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
            msg.u.auth.stat = param->auth_cmpl.stat;
            copy_name(msg.u.auth.name, sizeof(msg.u.auth.name), param->auth_cmpl.device_name,
                      strnlen(reinterpret_cast<const char*>(param->auth_cmpl.device_name),
                              ESP_BT_GAP_MAX_BDNAME_LEN));
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            // Legacy pairing: answer inline with the configured PIN.
            std::string pin;
            {
                std::lock_guard<std::mutex> lock(self.pin_mutex_);
                pin = self.pin_;
            }
            esp_bt_pin_code_t code = {0};
            uint8_t len = static_cast<uint8_t>(std::min<size_t>(pin.size(), ESP_BT_PIN_CODE_LEN));
            memcpy(code, pin.data(), len);
            if (param->pin_req.min_16_digit) {
                for (uint8_t i = len; i < 16; ++i) {
                    code[i] = '0';
                }
                len = 16;
            }
            ESP_LOGI(TAG, "Legacy PIN requested; replying with the configured PIN");
            esp_bt_gap_pin_reply(param->pin_req.bda, true, len, code);
            return;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            // Numeric comparison with no display: accept (equivalent to Just Works).
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            return;
        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGW(TAG, "Speaker asked for a passkey; this bridge has no keypad, rejecting");
            esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
            return;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP passkey notification: %06" PRIu32, param->key_notif.passkey);
            return;
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            // Standard HCI Read RSSI: on BR/EDR this is dB relative to the controller's golden
            // receive power range (0 = inside it), as Linux's Get Connection Information reports.
            memcpy(msg.u.rssi.bda, param->read_rssi_delta.bda, ESP_BD_ADDR_LEN);
            msg.u.rssi.stat = param->read_rssi_delta.stat;
            msg.u.rssi.rssi = param->read_rssi_delta.rssi_delta;
            break;
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            break;
        default:
            return;  // not needed
    }
    self.post(msg, pdMS_TO_TICKS(20));
}

void BtManager::a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    if (event == ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT) {
        // This pointer is only valid during the callback. Log before returning.
        const auto& caps = param->a2d_report_snk_all_codec_caps_stat;
        for (uint8_t i = 0; i < caps.sep_num; ++i) {
            const auto& codec = caps.sep_mcc[i].mcc;
            const char* name = codec.type == ESP_A2D_MCT_SBC ? "SBC" :
                               codec.type == ESP_A2D_MCT_M24 ? "AAC" :
                               repo_ldac_cie_known(codec.cie.vendor_info) ? "LDAC" : "vendor";
            ESP_LOGI(TAG, "Speaker endpoint %u: %s (codec 0x%x)", caps.sep_mcc[i].seid, name, codec.type);
            if (codec.type == ESP_A2D_MCT_NON_A2DP) {
                const auto* v = codec.cie.vendor_info;
                ESP_LOGI(TAG, "Vendor caps: %02x %02x %02x %02x %02x %02x rates=%02x channels=%02x",
                         v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            }
        }
        return;
    }
    Msg msg{};
    msg.kind = Kind::A2D;
    msg.event = static_cast<uint16_t>(event);
    if (param != nullptr) {
        msg.u.a2d = *param;
    }
    instance().post(msg, pdMS_TO_TICKS(20));
}

void BtManager::ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        case ESP_AVRC_CT_PROF_STATE_EVT:
            break;
        default:
            return;  // metadata / cover art carry pointers and are not used
    }
    Msg msg{};
    msg.kind = Kind::CT;
    msg.event = static_cast<uint16_t>(event);
    msg.u.ct = *param;
    instance().post(msg, pdMS_TO_TICKS(20));
}

void BtManager::tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        case ESP_AVRC_TG_PROF_STATE_EVT:
            break;
        default:
            return;
    }
    Msg msg{};
    msg.kind = Kind::TG;
    msg.event = static_cast<uint16_t>(event);
    msg.u.tg = *param;
    instance().post(msg, pdMS_TO_TICKS(20));
}

void BtManager::post(const Msg& msg, TickType_t wait) {
    if (queue_ == nullptr || xQueueSend(queue_, &msg, wait) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped kind=%d event=%d", static_cast<int>(msg.kind),
                 msg.event);
    }
}

// ---------------------------------------------------------------------------------------------
// bt_app task
// ---------------------------------------------------------------------------------------------

void BtManager::task_entry(void* arg) {
    static_cast<BtManager*>(arg)->task_loop();
}

void BtManager::task_loop() {
    refresh_bonded();
    next_attempt_us_ = esp_timer_get_time() + 2 * US_PER_S;  // let Wi-Fi come up first
    publish("started");
    int64_t next_tick = 0;
    Msg msg;
    while (true) {
        if (xQueueReceive(queue_, &msg, pdMS_TO_TICKS(250)) == pdTRUE) {
            handle(msg);
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick = now + 500000;
            tick();
        }
    }
}

void BtManager::handle(const Msg& msg) {
    switch (msg.kind) {
        case Kind::GAP:
            handle_gap(msg);
            break;
        case Kind::A2D:
            handle_a2d(msg);
            break;
        case Kind::CT:
            handle_ct(msg);
            break;
        case Kind::TG:
            handle_tg(msg);
            break;
        case Kind::CMD:
            handle_cmd(msg);
            break;
    }
}

bool BtManager::want_media() const {
    return conn_ == Conn::CONNECTED && !power_saved_;
}

int64_t BtManager::backoff_us(int attempt) const {
    // reconnect_policy.ReconnectPolicy.delay_for():
    //   min(check_interval * 2 ** max(0, attempt - 3), MAX_RECONNECT_DELAY_S)
    int shift = std::max(0, attempt - 3);
    int64_t base = static_cast<int64_t>(check_interval_s_) * US_PER_S;
    if (shift >= 20) {
        return MAX_RECONNECT_DELAY_US;
    }
    return std::min(base << shift, MAX_RECONNECT_DELAY_US);
}

void BtManager::start_connect() {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", target_[0], target_[1], target_[2],
             target_[3], target_[4], target_[5]);
    ESP_LOGI(TAG, "Connecting to %s (%s), attempt %d", target_name_.c_str(), mac, attempts_ + 1);
    esp_bd_addr_t bda;
    memcpy(bda, target_.data(), ESP_BD_ADDR_LEN);
    connect_asap_ = false;
    link_bda_ = target_;
    esp_err_t err = esp_a2d_source_connect(bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_a2d_source_connect: %s", esp_err_to_name(err));
        schedule_retry_after_failure();
        return;
    }
    conn_ = Conn::CONNECTING;
    connect_started_us_ = esp_timer_get_time();
    publish("connecting");
}

void BtManager::schedule_retry_after_failure() {
    ++attempts_;
    if (max_fails_ > 0 && attempts_ >= max_fails_) {
        gave_up_ = true;
        ESP_LOGW(TAG, "Giving up after %d failed reconnects (BT_MAX_RECONNECT_FAILS)", attempts_);
        publish("gave up reconnecting");
        return;
    }
    int64_t delay = backoff_us(attempts_);
    next_attempt_us_ = esp_timer_get_time() + delay;
    ESP_LOGI(TAG, "Reconnect attempt %d failed; next in %d s", attempts_,
             static_cast<int>(delay / US_PER_S));
}

void BtManager::tick() {
    const int64_t now = esp_timer_get_time();

    if (!tg_filter_set_) {
        apply_tg_filter();  // no-op until AVRCP TG init has completed
    }

    if (registered_seps_ == required_seps_ && conn_ == Conn::DISCONNECTED && has_target_ && !standby_ && !gave_up_ && !scanning_ &&
        now >= next_attempt_us_) {
        start_connect();
    }

    if (conn_ == Conn::CONNECTING && now - connect_started_us_ > CONNECT_TIMEOUT_US) {
        ESP_LOGW(TAG, "Connect timed out");
        conn_ = Conn::DISCONNECTED;
        schedule_retry_after_failure();
    }

    if (conn_ == Conn::CONNECTED) {
        if (media_ == Media::STARTED) {
            if (encoder_.failed() || !encoder_.start(audio_connection_, audio_mtu_,
                    [this](uint8_t* data, size_t bytes) { return read_pcm(data, bytes); },
                    callbacks_.frames_played)) {
                ESP_LOGE(TAG, "Bluetooth %s encoder failed; reconnecting", encoder_.codec_name());
                if (strcmp(encoder_.codec_name(), "LDAC") == 0) {
                    ESP_LOGE(TAG, "If this repeats, set LDAC quality to Off to use AAC/SBC instead");
                }
                encoder_.stop();
                esp_a2d_source_disconnect(peer_.data());
                conn_ = Conn::DISCONNECTING;
                publish("audio encoder failed");
                return;
            }
        } else {
            encoder_.stop();
        }
        if (want_media() && media_ == Media::IDLE && now >= next_media_us_) {
            if (esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) == ESP_OK) {
                media_ = Media::CHECKING;
                media_cmd_us_ = now;
            } else {
                next_media_us_ = now + 2 * US_PER_S;
            }
        } else if (!want_media() && media_ == Media::STARTED) {
            if (esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND) == ESP_OK) {
                media_ = Media::SUSPENDING;
                media_cmd_us_ = now;
            }
        }
        if ((media_ == Media::CHECKING || media_ == Media::STARTING || media_ == Media::SUSPENDING) &&
            now - media_cmd_us_ > MEDIA_CMD_TIMEOUT_US) {
            ESP_LOGW(TAG, "Media command timed out");
            media_ = (media_ == Media::SUSPENDING) ? Media::STARTED : Media::IDLE;
            next_media_us_ = now + 2 * US_PER_S;
        }
        if (now >= next_rssi_us_) {
            next_rssi_us_ = now + RSSI_POLL_US;
            esp_bd_addr_t bda;
            memcpy(bda, peer_.data(), ESP_BD_ADDR_LEN);
            esp_bt_gap_read_rssi_delta(bda);
        }
    }

    publish();
}

void BtManager::handle_gap(const Msg& msg) {
    switch (msg.event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            const DiscResult& r = msg.u.disc;
            auto bda = to_array(r.bda);
            auto it = std::find_if(scan_results_.begin(), scan_results_.end(),
                                   [&](const BtScanResult& s) { return s.bda == bda; });
            if (it == scan_results_.end()) {
                if (scan_results_.size() >= MAX_SCAN_RESULTS) {
                    return;
                }
                scan_results_.push_back({});
                it = scan_results_.end() - 1;
                it->bda = bda;
            }
            if (r.name[0] != '\0') {
                it->name = r.name;
            }
            if (r.cod != 0) {
                it->cod = r.cod;
            }
            if (r.rssi != -127) {
                it->rssi = r.rssi;
            }
            it->audio = esp_bt_gap_is_valid_cod(it->cod) &&
                        (esp_bt_gap_get_cod_major_dev(it->cod) == ESP_BT_COD_MAJOR_DEV_AV ||
                         (esp_bt_gap_get_cod_srvc(it->cod) & ESP_BT_COD_SRVC_RENDERING) != 0);
            it->bonded = std::find(bonded_.begin(), bonded_.end(), bda) != bonded_.end();
            publish();
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (msg.u.disc_state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                scanning_ = false;
                ESP_LOGI(TAG, "Scan finished, %u devices", static_cast<unsigned>(scan_results_.size()));
                publish("scan finished");
            } else {
                scanning_ = true;
                publish("scanning");
            }
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (msg.u.auth.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Paired with %s", msg.u.auth.name);
                refresh_bonded();
                publish("paired");
            } else {
                ESP_LOGW(TAG, "Pairing failed, status %d", msg.u.auth.stat);
                publish("pairing failed");
            }
            break;
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            if (conn_ != Conn::CONNECTED || !bda_equal(msg.u.rssi.bda, peer_.data())) break;
            if (msg.u.rssi.stat == ESP_BT_STATUS_SUCCESS) {
                rssi_valid_ = true;
                rssi_ = msg.u.rssi.rssi;
            } else {
                rssi_valid_ = false;
            }
            break;
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            refresh_bonded();
            publish("bond removed");
            break;
        default:
            break;
    }
}

void BtManager::handle_a2d(const Msg& msg) {
    const esp_a2d_cb_param_t& a2d = msg.u.a2d;
    const int64_t now = esp_timer_get_time();

    switch (msg.event) {
        case ESP_A2D_SEP_REG_STATE_EVT: {
            const auto& sep = a2d.a2d_sep_reg_stat;
            if (sep.seid > 2) break;
            if (sep.reg_state == ESP_A2D_SEP_REG_SUCCESS) {
                registered_seps_ |= 1U << sep.seid;
            } else {
                registered_seps_ &= ~(1U << sep.seid);
                ESP_LOGE(TAG, "Codec endpoint %u registration failed: %d", sep.seid, sep.reg_state);
            }
            break;
        }
        case ESP_A2D_CONNECTION_STATE_EVT: {
            const auto state = a2d.conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                if (!has_target_ || !bda_equal(a2d.conn_stat.remote_bda, target_.data())) {
                    ESP_LOGW(TAG, "Rejecting A2DP connection from a device that is not the selected speaker");
                    esp_bd_addr_t bda;
                    memcpy(bda, a2d.conn_stat.remote_bda, ESP_BD_ADDR_LEN);
                    esp_a2d_source_disconnect(bda);
                    conn_ = Conn::DISCONNECTING;
                    return;
                }
                if (pending_disconnect_) {
                    // Standby was requested while this connect was in flight. Bluedroid drops a
                    // disconnect sent during OPENING (btc_av.c has no DISCONNECT_REQ case there),
                    // so issue it now that the link is open.
                    pending_disconnect_ = false;
                    ESP_LOGI(TAG, "Connected during a standby request; releasing the speaker");
                    esp_bd_addr_t bda;
                    memcpy(bda, a2d.conn_stat.remote_bda, ESP_BD_ADDR_LEN);
                    esp_a2d_source_disconnect(bda);
                    conn_ = Conn::DISCONNECTING;
                    return;
                }
                if (standby_) {
                    ESP_LOGI(TAG, "Speaker reconnected on its own; leaving standby");
                    standby_ = false;
                }
                conn_ = Conn::CONNECTED;
                peer_ = target_;
                attempts_ = 0;
                gave_up_ = false;
                media_ = Media::IDLE;
                next_media_us_ = now + 300000;
                next_rssi_us_ = now + US_PER_S;
                rssi_valid_ = false;
                delay_report_ms_ = -1;
                audio_connection_ = a2d.conn_stat.conn_hdl;
                audio_mtu_ = a2d.conn_stat.audio_mtu;
                connected_flag_.store(true);
                ESP_LOGI(TAG, "A2DP connected to %s", target_name_.c_str());
                publish("connected");
                if (callbacks_.connection) {
                    callbacks_.connection(true);
                }
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                encoder_.clear();
                audio_connection_ = 0;
                audio_mtu_ = 0;
                const Conn previous = conn_;
                const bool was_up = connected_flag_.exchange(false);
                const bool stale_attempt = previous == Conn::CONNECTING &&
                                           !(has_target_ && bda_equal(a2d.conn_stat.remote_bda, target_.data()));
                conn_ = Conn::DISCONNECTED;
                media_ = Media::IDLE;
                rc_connected_ = false;
                abs_volume_ = false;
                last_sent_abs_ = -1;
                rssi_valid_ = false;
                pending_disconnect_ = false;
                mono_.store(false);
                apply_volume();
                if (previous == Conn::CONNECTING && !stale_attempt && !connect_asap_) {
                    schedule_retry_after_failure();
                } else if (previous == Conn::CONNECTING) {
                    // An attempt to a speaker that is no longer the target, or one superseded by
                    // a deliberate switch: not a failure of the current target. Dial it now.
                    next_attempt_us_ = now;
                } else if (previous == Conn::CONNECTED || previous == Conn::DISCONNECTING) {
                    attempts_ = 0;
                    // After a deliberate switch (new speaker / reconnect / wake) dial at once;
                    // after a drop give the speaker one check interval to come back by itself.
                    next_attempt_us_ = connect_asap_
                                           ? now
                                           : now + static_cast<int64_t>(check_interval_s_) * US_PER_S;
                    ESP_LOGI(TAG, "A2DP disconnected (%s)",
                             a2d.conn_stat.disc_rsn == ESP_A2D_DISC_RSN_ABNORMAL ? "link lost" : "normal");
                }
                // Bluedroid reports DISCONNECTING first for a disconnect we start, so decide
                // from whether the target link was up, not from the previous state.
                if (was_up && callbacks_.connection) {
                    callbacks_.connection(false);
                }
                publish("disconnected");
            } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                if (conn_ == Conn::DISCONNECTED) {
                    conn_ = Conn::CONNECTING;  // speaker-initiated
                    connect_started_us_ = now;
                    link_bda_ = to_array(a2d.conn_stat.remote_bda);
                }
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                conn_ = Conn::DISCONNECTING;
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT:
            if (a2d.audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                media_ = Media::STARTED;
                publish("streaming");
            } else {
                encoder_.stop();
                // Suspended (by us or by the speaker). Restart later if we still want media.
                media_ = Media::IDLE;
                next_media_us_ = now + 3 * US_PER_S;
                publish("stream suspended");
            }
            break;
        case ESP_A2D_AUDIO_CFG_EVT:
            if (!has_target_ || !bda_equal(a2d.audio_cfg.remote_bda, target_.data())) break;
            if (!encoder_.configure(a2d.audio_cfg.mcc)) {
                ESP_LOGE(TAG, "Cannot use negotiated Bluetooth codec; disconnecting");
                esp_a2d_source_disconnect(target_.data());
                conn_ = Conn::DISCONNECTING;
            }
            // Channel layout comes from the negotiated configuration, not sink capabilities.
            mono_.store(encoder_.mono());
            publish();
            break;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
            const auto cmd = a2d.media_ctrl_stat.cmd;
            const bool ok = a2d.media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
            if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && media_ == Media::CHECKING) {
                if (ok && want_media() && esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START) == ESP_OK) {
                    media_ = Media::STARTING;
                    media_cmd_us_ = now;
                } else {
                    media_ = Media::IDLE;
                    next_media_us_ = now + 2 * US_PER_S;
                }
            } else if (cmd == ESP_A2D_MEDIA_CTRL_START && media_ == Media::STARTING) {
                if (ok) {
                    media_ = Media::STARTED;
                } else {
                    ESP_LOGW(TAG, "Media start rejected");
                    media_ = Media::IDLE;
                    next_media_us_ = now + 2 * US_PER_S;
                }
            } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && media_ == Media::SUSPENDING) {
                media_ = ok ? Media::IDLE : Media::STARTED;
            }
            publish();
            break;
        }
        case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
            // delay_value is in 1/10 ms
            delay_report_ms_ = (a2d.a2d_report_delay_value_stat.delay_value + 5) / 10;
            ESP_LOGI(TAG, "Speaker reports %d ms output delay", delay_report_ms_);
            if (callbacks_.delay_report) {
                callbacks_.delay_report(static_cast<uint16_t>(delay_report_ms_));
            }
            publish();
            break;
        }
        case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
            const esp_a2d_mcc_t& mcc = a2d.a2d_report_snk_codec_caps_stat.mcc;
            if (mcc.type == ESP_A2D_MCT_SBC) {
                const uint8_t stereo_modes = ESP_A2D_SBC_CIE_CH_MODE_STEREO |
                                             ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO |
                                             ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL;
                const bool mono_only = (mcc.cie.sbc_info.ch_mode & stereo_modes) == 0;
                ESP_LOGI(TAG, "Speaker SBC caps: freq 0x%x ch_mode 0x%x bitpool %d-%d%s",
                         mcc.cie.sbc_info.samp_freq, mcc.cie.sbc_info.ch_mode,
                         mcc.cie.sbc_info.min_bitpool, mcc.cie.sbc_info.max_bitpool,
                         mono_only ? " (mono only)" : "");
            }
            break;
        }
        default:
            break;
    }
}

void BtManager::handle_ct(const Msg& msg) {
    const esp_avrc_ct_cb_param_t& rc = msg.u.ct;
    switch (msg.event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            rc_connected_ = rc.conn_stat.connected;
            if (rc_connected_) {
                esp_avrc_ct_send_get_rn_capabilities_cmd(TL_GET_CAPS);
            } else {
                abs_volume_ = false;
                last_sent_abs_ = -1;
                apply_volume();
            }
            publish();
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
            esp_avrc_rn_evt_cap_mask_t caps = rc.get_rn_caps_rsp.evt_set;
            abs_volume_ = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps,
                                                             ESP_AVRC_RN_VOLUME_CHANGE);
            ESP_LOGI(TAG, "Speaker %s AVRCP absolute volume",
                     abs_volume_ ? "supports" : "does not support");
            if (abs_volume_) {
                esp_avrc_ct_send_register_notification_cmd(TL_RN_VOLUME, ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
            last_sent_abs_ = -1;
            apply_volume();
            publish();
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (rc.change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                const int abs = rc.change_ntf.event_parameter.volume & 0x7F;
                // Echo of our own SetAbsoluteVolume: same value (+-1 for speakers that round)
                // shortly after we sent it. Anything else is a change made on the speaker.
                const bool echo = last_sent_abs_ >= 0 && std::abs(abs - last_sent_abs_) <= 1 &&
                                  esp_timer_get_time() - last_abs_cmd_us_ < ABS_VOLUME_ECHO_US;
                if (!echo && abs != last_sent_abs_) {
                    volume_ = from_avrcp(static_cast<uint8_t>(abs));
                    ESP_LOGI(TAG, "Volume changed on the speaker: %d%%", volume_);
                    if (callbacks_.speaker_volume) {
                        callbacks_.speaker_volume(volume_);
                    }
                }
                last_sent_abs_ = abs;
                // Notifications are one-shot: register again.
                esp_avrc_ct_send_register_notification_cmd(TL_RN_VOLUME, ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
            break;
        case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
            last_sent_abs_ = rc.set_volume_rsp.volume & 0x7F;
            break;
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
            ESP_LOGI(TAG, "Speaker AVRCP features 0x%" PRIx32 ", TG flags 0x%x",
                     rc.rmt_feats.feat_mask, rc.rmt_feats.tg_feat_flag);
            break;
        default:
            break;
    }
}

void BtManager::handle_tg(const Msg& msg) {
    const esp_avrc_tg_cb_param_t& rc = msg.u.tg;
    switch (msg.event) {
        case ESP_AVRC_TG_PROF_STATE_EVT:
            apply_tg_filter();
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            if (rc.psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
                ESP_LOGI(TAG, "Speaker button: AVRCP key 0x%02x", rc.psth_cmd.key_code);
                if (callbacks_.passthrough) {
                    callbacks_.passthrough(rc.psth_cmd.key_code);
                }
            }
            break;
        default:
            break;
    }
}

void BtManager::apply_volume() {
    if (abs_volume_ && rc_connected_) {
        // The speaker scales; we send full-scale PCM (or silence when muted).
        gain_q16_.store(muted_ ? 0 : 65536);
        const uint8_t abs = to_avrcp(volume_);
        if (last_sent_abs_ != abs) {
            if (esp_avrc_ct_send_set_absolute_volume_cmd(TL_SET_VOLUME, abs) == ESP_OK) {
                last_sent_abs_ = abs;
                last_abs_cmd_us_ = esp_timer_get_time();
            }
        }
    } else {
        gain_q16_.store(gain_for(volume_, muted_));
    }
}

void BtManager::refresh_bonded() {
    bonded_.clear();
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        return;
    }
    std::unique_ptr<esp_bd_addr_t[]> list(new esp_bd_addr_t[static_cast<size_t>(count)]);
    if (esp_bt_gap_get_bond_device_list(&count, list.get()) == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            bonded_.push_back(to_array(list[i]));
        }
    }
}

void BtManager::handle_cmd(const Msg& msg) {
    const CmdData& c = msg.u.cmd;
    const int64_t now = esp_timer_get_time();
    switch (c.type) {
        case Cmd::SET_TARGET: {
            auto bda = to_array(c.bda);
            if (conn_ != Conn::DISCONNECTED && link_bda_ != bda) {
                // Dropped by Bluedroid if the link is still opening; the CONNECTED handler then
                // rejects the old speaker because it is no longer the target.
                esp_bd_addr_t cur;
                memcpy(cur, link_bda_.data(), ESP_BD_ADDR_LEN);
                esp_a2d_source_disconnect(cur);
            }
            pending_disconnect_ = false;
            has_target_ = true;
            target_ = bda;
            target_name_ = c.name;
            attempts_ = 0;
            gave_up_ = false;
            standby_ = false;
            next_attempt_us_ = now;
            connect_asap_ = true;
            if (scanning_) {
                esp_bt_gap_cancel_discovery();
            }
            publish("speaker selected");
            break;
        }
        case Cmd::CLEAR_TARGET: {
            if (conn_ != Conn::DISCONNECTED) {
                // If still opening, the CONNECTED handler rejects it (no target any more).
                esp_bd_addr_t cur;
                memcpy(cur, link_bda_.data(), ESP_BD_ADDR_LEN);
                esp_a2d_source_disconnect(cur);
            }
            pending_disconnect_ = false;
            if (c.a != 0 && has_target_) {
                esp_bd_addr_t bda;
                memcpy(bda, target_.data(), ESP_BD_ADDR_LEN);
                esp_bt_gap_remove_bond_device(bda);
            }
            has_target_ = false;
            target_name_.clear();
            publish("speaker forgotten");
            break;
        }
        case Cmd::SCAN:
            if (!scanning_) {
                refresh_bonded();
                scan_results_.clear();
                esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LEN, 0);
                if (err == ESP_OK) {
                    scanning_ = true;
                    publish("scanning");
                } else {
                    ESP_LOGW(TAG, "Discovery failed to start: %s", esp_err_to_name(err));
                }
            }
            break;
        case Cmd::RECONNECT:
            standby_ = false;
            pending_disconnect_ = false;
            gave_up_ = false;
            attempts_ = 0;
            next_attempt_us_ = now;
            connect_asap_ = true;
            publish("reconnect requested");
            break;
        case Cmd::STANDBY:
            if (!standby_) {
                standby_ = true;
                if (conn_ == Conn::CONNECTED) {
                    esp_bd_addr_t cur;
                    memcpy(cur, peer_.data(), ESP_BD_ADDR_LEN);
                    esp_a2d_source_disconnect(cur);
                } else if (conn_ == Conn::CONNECTING) {
                    // Bluedroid ignores a disconnect while the link is opening; disconnect as
                    // soon as it opens (or not at all if the attempt fails).
                    pending_disconnect_ = true;
                }
                publish("standby");
            }
            break;
        case Cmd::WAKE:
            if (standby_) {
                standby_ = false;
                pending_disconnect_ = false;
                attempts_ = 0;
                gave_up_ = false;
                next_attempt_us_ = now;
                connect_asap_ = true;
                publish("waking");
            }
            break;
        case Cmd::POWER_SAVE:
            if (power_saved_ != (c.a != 0)) {
                power_saved_ = c.a != 0;
                if (!power_saved_) {
                    next_media_us_ = now;
                }
                publish(power_saved_ ? "power save: stream suspended" : "power save: stream resumed");
            }
            break;
        case Cmd::VOLUME:
            volume_ = static_cast<uint8_t>(std::clamp<int32_t>(c.a, 0, 100));
            muted_ = c.b != 0;
            apply_volume();
            break;
        case Cmd::POLICY:
            check_interval_s_ = static_cast<uint16_t>(std::max<int32_t>(5, c.a));
            max_fails_ = static_cast<uint16_t>(std::max<int32_t>(0, c.b));
            break;
        case Cmd::LDAC_QUALITY:
            encoder_.set_ldac_quality(static_cast<LdacQuality>(c.a));
            break;
    }
}

void BtManager::publish(const char* event) {
    const int64_t now = esp_timer_get_time();
    if (event != nullptr) {
        last_event_ = event;
    }
    BtStatus s;
    s.ready = registered_seps_ == required_seps_;
    s.local_mac = local_mac_;
    s.has_target = has_target_;
    if (has_target_) {
        s.target_mac = app_config::bda_to_string(target_);
        s.target_name = target_name_;
    }
    s.connected = conn_ == Conn::CONNECTED;
    s.streaming = s.connected && media_ == Media::STARTED;
    s.power_saved = power_saved_;
    s.standby = standby_;
    s.gave_up = gave_up_;
    s.reconnect_attempt = attempts_;
    if (has_target_ && conn_ == Conn::DISCONNECTED && !standby_ && !gave_up_) {
        s.next_attempt_in_s = static_cast<int>(std::max<int64_t>(0, next_attempt_us_ - now) / US_PER_S);
    }
    if (!has_target_) {
        s.state = "no_speaker";
    } else if (gave_up_) {
        s.state = "gave_up";
    } else if (standby_ && conn_ == Conn::DISCONNECTED) {
        s.state = "standby";
    } else {
        switch (conn_) {
            case Conn::DISCONNECTED:
                s.state = "disconnected";
                break;
            case Conn::CONNECTING:
                s.state = "connecting";
                break;
            case Conn::CONNECTED:
                s.state = s.streaming ? "streaming" : "connected";
                break;
            case Conn::DISCONNECTING:
                s.state = "disconnecting";
                break;
        }
    }
    s.scanning = scanning_;
    s.scan_results = scan_results_;
    for (const auto& b : bonded_) {
        s.bonded.push_back(app_config::bda_to_string(b));
    }
    s.abs_volume = abs_volume_ && rc_connected_;
    s.rssi_valid = rssi_valid_ && s.connected;
    s.rssi_delta_db = rssi_;
    s.delay_report_ms = delay_report_ms_;
    s.mono_sink = mono_.load();
    s.codec = s.connected ? encoder_.codec_name() : "";
    s.bitrate_bps = s.connected ? encoder_.bitrate_bps() : 0;
    s.encoder_metrics = encoder_.metrics();
    s.sample_rate = AudioPipe::sample_rate();
    s.pcm_bits = encoder_.pcm_bits();
    s.pcm_buffer_ms = pipe_ ? AudioPipe::frames_to_us(pipe_->buffered_bytes() / AudioPipe::bytes_per_frame()) / 1000 : 0;
    s.pcm_underrun_frames = pcm_underrun_frames_.load();
    s.last_event = last_event_;

    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = std::move(s);
}

// ---------------------------------------------------------------------------------------------
// Public commands
// ---------------------------------------------------------------------------------------------

void BtManager::set_target(const std::array<uint8_t, 6>& bda, const std::string& name) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::SET_TARGET;
    memcpy(msg.u.cmd.bda, bda.data(), ESP_BD_ADDR_LEN);
    copy_name(msg.u.cmd.name, sizeof(msg.u.cmd.name), reinterpret_cast<const uint8_t*>(name.data()),
              name.size());
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::clear_target(bool remove_bond) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::CLEAR_TARGET;
    msg.u.cmd.a = remove_bond ? 1 : 0;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::start_scan() {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::SCAN;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::reconnect_now() {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::RECONNECT;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::enter_standby() {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::STANDBY;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::exit_standby() {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::WAKE;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::set_power_save(bool suspended) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::POWER_SAVE;
    msg.u.cmd.a = suspended ? 1 : 0;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::set_volume(uint8_t volume, bool muted) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::VOLUME;
    msg.u.cmd.a = volume;
    msg.u.cmd.b = muted ? 1 : 0;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::set_policy(uint16_t check_interval_s, uint16_t max_reconnect_fails) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::POLICY;
    msg.u.cmd.a = check_interval_s;
    msg.u.cmd.b = max_reconnect_fails;
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::set_pin(const std::string& pin) {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    pin_ = pin;
}

void BtManager::set_ldac_quality(LdacQuality quality) {
    Msg msg{};
    msg.kind = Kind::CMD;
    msg.u.cmd.type = Cmd::LDAC_QUALITY;
    msg.u.cmd.a = static_cast<int32_t>(quality);
    post(msg, pdMS_TO_TICKS(200));
}

void BtManager::keepalive_burst(KeepAliveMethod method) {
    if (method == KeepAliveMethod::NONE) {
        return;
    }
    keepalive_method_.store(static_cast<uint8_t>(method));
    keepalive_frames_.store(AudioPipe::sample_rate());
}

BtStatus BtManager::status() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

bool BtManager::consuming() const {
    int64_t last = last_data_cb_us_.load(std::memory_order_relaxed);
    return last != 0 && esp_timer_get_time() - last < CONSUMING_WINDOW_US;
}

// ---------------------------------------------------------------------------------------------
// A2DP PCM data path (encoder worker)
// ---------------------------------------------------------------------------------------------

void BtManager::apply_gain(int16_t* samples, size_t count) {
    const uint32_t gain = gain_q16_.load(std::memory_order_relaxed);
    if (gain >= 65536) {
        return;
    }
    if (gain == 0) {
        memset(samples, 0, count * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i]) * static_cast<int32_t>(gain)) >> 16);
    }
}

void BtManager::fill_idle(uint8_t* dst, size_t bytes, int channels) {
    const uint8_t bits = encoder_.pcm_bits();
    const size_t width = bits / 8, frame_bytes = channels * width;
    const size_t frames = bytes / frame_bytes;
    const uint32_t period = AudioPipe::sample_rate() / 2;
    uint32_t burst = keepalive_frames_.load(std::memory_order_relaxed);
    const bool tone = keepalive_method_.load(std::memory_order_relaxed) ==
                      static_cast<uint8_t>(KeepAliveMethod::INFRASOUND);
    size_t i = 0;
    if (burst > 0) {
        const size_t n = std::min<size_t>(frames, burst);
        for (; i < n; ++i) {
            int32_t v = 0;
            if (tone) {
                v = static_cast<int32_t>(INFRASOUND_AMPLITUDE * 256 *
                    sinf(TWO_PI * static_cast<float>(infrasound_phase_) / period));
                infrasound_phase_ = (infrasound_phase_ + 1) % period;
            }
            for (int c = 0; c < channels; ++c) pcm::write(dst + i * frame_bytes + c * width, bits, v);
        }
        keepalive_frames_.store(burst - static_cast<uint32_t>(n), std::memory_order_relaxed);
    }
    memset(dst + i * frame_bytes, 0, bytes - i * frame_bytes);
}

uint32_t BtManager::read_pcm(uint8_t* data, size_t len) {
    last_data_cb_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    const bool mono = mono_.load(std::memory_order_relaxed);
    const uint8_t bits = encoder_.pcm_bits();
    const size_t output_frame = (mono ? 1 : 2) * (bits / 8);
    const size_t wanted = len / output_frame;
    size_t done = 0;
    const uint32_t gain = gain_q16_.load(std::memory_order_relaxed);
    if (!mono && bits == AudioPipe::bits_per_sample()) {
        const size_t got = pipe_ ? pipe_->read(data, wanted * output_frame) : 0;
        done = got / output_frame;
        if (bits == 16) apply_gain(reinterpret_cast<int16_t*>(data), got / 2);
        else if (gain != 65536) pcm::convert(data, data, done, bits, bits, false, gain);
    } else {
        while (done < wanted && pipe_) {
            const size_t chunk = std::min(wanted - done, SCRATCH_FRAMES);
            const size_t got = pipe_->read(g_scratch, chunk * AudioPipe::bytes_per_frame()) / AudioPipe::bytes_per_frame();
            pcm::convert(data + done * output_frame, g_scratch, got,
                         AudioPipe::bits_per_sample(), bits, mono, gain);
            done += got;
            if (got < chunk) break;
        }
    }
    if (done < wanted) {
        fill_idle(data + done * output_frame, len - done * output_frame, mono ? 1 : 2);
        if (audio_active_.load(std::memory_order_relaxed)) pcm_underrun_frames_ += wanted - done;
    }
    return static_cast<uint32_t>(done);
}
