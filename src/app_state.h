// Cross-task plumbing:
//  - a work queue run on the main loop, which owns the Sendspin client (most of its API is
//    main-loop only), so web handlers and Bluetooth events never call it directly;
//  - the Sendspin-side status snapshot the web UI reads.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app_state {

/// Queues fn to run on the main loop. Thread-safe.
void post(std::function<void()> fn);

/// Runs everything queued so far. Main loop only.
void run_pending();

struct SendspinStatus {
    bool enabled{false};          // client was created and started
    std::string disabled_reason;  // why not, when !enabled
    std::string client_id;
    bool connected{false};
    bool paired{false};
    bool time_synced{false};
    bool unpaired_access{false};
    std::string server_name;
    std::string server_id;
    std::string group_name;
    std::string playback_state;  // "playing", "stopped" or ""
    bool stream_active{false};
    std::string stream_codec;
    uint8_t volume{0};
    bool muted{false};
    uint16_t output_delay_ms{0};
    // Metadata
    std::string title;
    std::string artist;
    std::string album;
    uint32_t progress_ms{0};
    uint32_t duration_ms{0};
    // Controller
    std::vector<std::string> supported_commands;
    bool shuffle{false};
    std::string repeat;
    // Pairing UI
    std::string pairing_code;
    bool pairing_window_prompt{false};
    std::string pairing_event;
    // Idle machine
    std::string idle_state;
};

void set_sendspin(const SendspinStatus& status);
SendspinStatus get_sendspin();

}  // namespace app_state
