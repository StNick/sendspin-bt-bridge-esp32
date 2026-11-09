// PCM hand-off between the Sendspin sync task (producer, on_audio_write) and the Bluetooth
// LDAC/AAC/SBC encoder worker (consumer).
//
// Format is selected once at boot: 44.1/48 kHz, packed 16/24-bit stereo.
// Sendspin and Bluetooth advertise the same rate; AAC/SBC convert depth to 16-bit.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pcm_format.h"

class AudioPipe {
public:
    static constexpr uint8_t CHANNELS = 2;
    static bool configure_format(uint32_t rate, uint8_t bits) {
        if (!pcm::valid_format(rate, bits)) return false;
        sample_rate_ = rate;
        bits_ = bits;
        return true;
    }
    static uint32_t sample_rate() { return sample_rate_; }
    static uint8_t bits_per_sample() { return bits_; }
    static size_t bytes_per_frame() { return CHANNELS * (bits_ / 8); }

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
        return static_cast<int64_t>(frames) * 1000000LL / sample_rate_;
    }

private:
    // Immutable after app_main configures the pipeline, before tasks start.
    inline static uint32_t sample_rate_{44100};
    inline static uint8_t bits_{16};
    uint8_t* buffer_{nullptr};
    size_t capacity_{0};
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t fill_{0};
    mutable SemaphoreHandle_t mutex_{nullptr};
    SemaphoreHandle_t space_signal_{nullptr};
};
