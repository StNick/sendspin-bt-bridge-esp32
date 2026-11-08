// Web UI and REST API (port 80): the role of sendspin-bt-bridge's Flask app, scaled to one
// speaker. Every handler that touches the Sendspin client goes through WebActions, which the
// main loop implements, because most of the client API is main-loop only.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "app_config.h"
#include "esp_err.h"

struct WebActions {
    std::function<bool(const std::string& command)> transport;  // play, pause, stop, next, previous
    std::function<void(uint8_t volume)> set_volume;
    std::function<void(bool muted)> set_muted;
    std::function<void(uint16_t delay_ms)> set_delay;
    std::function<void()> pairing_confirm;
    std::function<void()> pairing_cancel;
    std::function<void()> speaker_selected;  // a different speaker was chosen
    std::function<void(const AppConfig& before, const AppConfig& after)> settings_changed;
};

namespace web_server {

esp_err_t start(WebActions actions);

}  // namespace web_server
