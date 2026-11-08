#include "log_buffer.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

SemaphoreHandle_t g_mutex = nullptr;
vprintf_like_t g_previous = nullptr;
char* g_ring = nullptr;
size_t g_capacity = 0;
size_t g_head = 0;   // next write position
size_t g_size = 0;   // bytes held (<= capacity)
char g_line[256];    // formatting scratch, guarded by g_mutex

void append_locked(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        g_ring[g_head] = data[i];
        g_head = (g_head + 1) % g_capacity;
        if (g_size < g_capacity) {
            ++g_size;
        }
    }
}

int log_hook(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int ret = g_previous != nullptr ? g_previous(fmt, args) : vprintf(fmt, args);

    // Never block a logging task: skip the copy when the buffer is busy or we are in an ISR.
    if (g_mutex != nullptr && !xPortInIsrContext() && xSemaphoreTake(g_mutex, 0) == pdTRUE) {
        int n = vsnprintf(g_line, sizeof(g_line), fmt, copy);
        if (n > 0) {
            size_t len = static_cast<size_t>(n) < sizeof(g_line) ? static_cast<size_t>(n)
                                                                 : sizeof(g_line) - 1;
            // A truncated line still ends with a newline so lines stay separated.
            if (static_cast<size_t>(n) >= sizeof(g_line)) {
                g_line[len - 1] = '\n';
            }
            append_locked(g_line, len);
        }
        xSemaphoreGive(g_mutex);
    }
    va_end(copy);
    return ret;
}

}  // namespace

namespace log_buffer {

void init(size_t capacity_bytes) {
    if (g_mutex != nullptr || capacity_bytes == 0) {
        return;
    }
    g_ring = static_cast<char*>(heap_caps_malloc(capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_ring == nullptr) {
        g_ring = static_cast<char*>(heap_caps_malloc(capacity_bytes, MALLOC_CAP_8BIT));
    }
    if (g_ring == nullptr) {
        return;
    }
    g_capacity = capacity_bytes;
    g_mutex = xSemaphoreCreateMutex();
    if (g_mutex == nullptr) {
        heap_caps_free(g_ring);
        g_ring = nullptr;
        return;
    }
    g_previous = esp_log_set_vprintf(log_hook);
}

std::string dump() {
    std::string out;
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return out;
    }
    out.reserve(g_size);
    size_t start = (g_size < g_capacity) ? 0 : g_head;
    for (size_t i = 0; i < g_size; ++i) {
        out.push_back(g_ring[(start + i) % g_capacity]);
    }
    bool wrapped = g_size == g_capacity;
    xSemaphoreGive(g_mutex);

    if (wrapped) {
        // Drop the partial first line left by the overwrite.
        size_t nl = out.find('\n');
        if (nl != std::string::npos) {
            out.erase(0, nl + 1);
        }
    }
    return out;
}

}  // namespace log_buffer
