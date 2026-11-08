#include "audio_pipe.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {
const char* TAG = "audio_pipe";

TickType_t ms_to_ticks(uint32_t ms) {
    if (ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    TickType_t t = pdMS_TO_TICKS(ms);
    return (ms != 0 && t == 0) ? 1 : t;
}
}  // namespace

bool AudioPipe::init(size_t capacity_bytes) {
    capacity_ = capacity_bytes - (capacity_bytes % BYTES_PER_FRAME);
    if (capacity_ == 0) {
        return false;
    }
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer_ == nullptr) {
        buffer_ = static_cast<uint8_t*>(heap_caps_malloc(capacity_, MALLOC_CAP_8BIT));
    }
    mutex_ = xSemaphoreCreateMutex();
    space_signal_ = xSemaphoreCreateBinary();
    if (buffer_ == nullptr || mutex_ == nullptr || space_signal_ == nullptr) {
        ESP_LOGE(TAG, "Allocation failed (%u bytes)", static_cast<unsigned>(capacity_));
        return false;
    }
    return true;
}

size_t AudioPipe::write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if (buffer_ == nullptr) {
        return 0;
    }
    len -= len % BYTES_PER_FRAME;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = ms_to_ticks(timeout_ms);
    size_t written = 0;

    while (written < len) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        size_t space = capacity_ - fill_;
        size_t chunk = std::min(space, len - written);
        chunk -= chunk % BYTES_PER_FRAME;
        if (chunk > 0) {
            size_t first = std::min(chunk, capacity_ - write_pos_);
            memcpy(buffer_ + write_pos_, data + written, first);
            if (chunk > first) {
                memcpy(buffer_, data + written + first, chunk - first);
            }
            write_pos_ = (write_pos_ + chunk) % capacity_;
            fill_ += chunk;
        }
        xSemaphoreGive(mutex_);
        written += chunk;

        if (written >= len) {
            break;
        }
        if (chunk == 0) {
            if (timeout != portMAX_DELAY) {
                TickType_t elapsed = xTaskGetTickCount() - start;
                if (elapsed >= timeout) {
                    break;
                }
                xSemaphoreTake(space_signal_, timeout - elapsed);
            } else {
                xSemaphoreTake(space_signal_, portMAX_DELAY);
            }
        }
    }
    return written;
}

size_t AudioPipe::read(uint8_t* dst, size_t len) {
    if (buffer_ == nullptr) {
        return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t n = std::min(len, fill_);
    n -= n % BYTES_PER_FRAME;
    if (n > 0) {
        size_t first = std::min(n, capacity_ - read_pos_);
        memcpy(dst, buffer_ + read_pos_, first);
        if (n > first) {
            memcpy(dst + first, buffer_, n - first);
        }
        read_pos_ = (read_pos_ + n) % capacity_;
        fill_ -= n;
    }
    xSemaphoreGive(mutex_);
    if (n > 0) {
        xSemaphoreGive(space_signal_);
    }
    return n;
}

void AudioPipe::clear() {
    if (buffer_ == nullptr) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    read_pos_ = 0;
    write_pos_ = 0;
    fill_ = 0;
    xSemaphoreGive(mutex_);
    xSemaphoreGive(space_signal_);
}

size_t AudioPipe::buffered_bytes() const {
    if (buffer_ == nullptr) {
        return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t n = fill_;
    xSemaphoreGive(mutex_);
    return n;
}
