// PCM hand-off between the Sendspin sync task (producer, on_audio_write) and the Bluetooth
// A2DP source data callback (consumer).
//
// Format is fixed at 44.1 kHz / 16-bit / stereo: Bluedroid's A2DP source advertises only
// 44.1 kHz SBC (bta_av_co_sbc_caps), and the bridge advertises only that PCM format to Sendspin,
// so the server resamples and no resampler is needed on the device.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class AudioPipe {
public:
    static constexpr uint32_t SAMPLE_RATE = 44100;
    static constexpr uint8_t CHANNELS = 2;
    static constexpr uint8_t BITS_PER_SAMPLE = 16;
    static constexpr size_t BYTES_PER_FRAME = CHANNELS * (BITS_PER_SAMPLE / 8);

    /// Allocates the ring (PSRAM first). capacity is rounded down to whole frames.
    bool init(size_t capacity_bytes);

    /// Producer: copies whole frames, waiting up to timeout_ms for space.
    /// Returns bytes written (always a whole number of frames).
    size_t write(const uint8_t* data, size_t len, uint32_t timeout_ms);

    /// Consumer: copies up to len bytes of whole frames without blocking. Returns bytes read.
    size_t read(uint8_t* dst, size_t len);

    /// Drops everything buffered.
    void clear();

    size_t buffered_bytes() const;
    size_t capacity_bytes() const {
        return capacity_;
    }

    /// Frames per second duration helper.
    static int64_t frames_to_us(uint32_t frames) {
        return static_cast<int64_t>(frames) * 1000000LL / SAMPLE_RATE;
    }

private:
    uint8_t* buffer_{nullptr};
    size_t capacity_{0};
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t fill_{0};
    mutable SemaphoreHandle_t mutex_{nullptr};
    SemaphoreHandle_t space_signal_{nullptr};
};
