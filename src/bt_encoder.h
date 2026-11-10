#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "app_config.h"

#include "esp_a2dp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Controlled by bt_app; the worker exclusively owns the encoder and PCM consumer.
class BtEncoder {
public:
    struct Metrics {
        uint32_t encode_avg_us, encode_max_us, late_batches, send_retries, dropped_packets;
    };
    using ReadPcm = std::function<uint32_t(uint8_t*, size_t)>;
    using Played = std::function<void(uint32_t, int64_t)>;

    static esp_err_t register_endpoints(bool ldac);
    bool configure(const esp_a2d_mcc_t& codec);
    bool start(esp_a2d_conn_hdl_t connection, uint16_t mtu, ReadPcm read, Played played);
    void stop();
    void clear();
    const char* codec_name() const;
    uint32_t bitrate_bps() const { return bitrate_bps_; }
    uint8_t pcm_bits() const { return pcm_bits_; }
    void set_ldac_quality(LdacQuality quality);
    Metrics metrics() const;
    bool mono() const { return channels_ == 1; }
    bool failed() const { return task_active_ && !running_.load(); }

private:
    static void task_entry(void* arg);
    void stream();
    void close();

    void* encoder_{nullptr};
    bool aac_{false};
    bool ldac_{false};
    LdacQuality ldac_quality_{LdacQuality::STANDARD};
    uint32_t bitrate_bps_{0};
    uint8_t pcm_bits_{16};
    int channels_{2};
    int input_size_{0};
    int output_size_{0};
    unsigned packet_frames_{1};
    uint8_t* pcm_{nullptr};
    uint8_t* encoded_{nullptr};
    esp_a2d_conn_hdl_t connection_{0};
    uint16_t mtu_{0};
    ReadPcm read_;
    Played played_;
    std::atomic<bool> running_{false};
    bool task_active_{false};
    SemaphoreHandle_t stopped_{nullptr};
    uint32_t timestamp_{0};
    std::atomic<uint32_t> encode_avg_us_{0}, encode_max_us_{0}, late_batches_{0};
    std::atomic<uint32_t> send_retries_{0}, dropped_packets_{0};
};
