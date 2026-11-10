"""Run encoder/transport regression checks without a board.

uv run --with ziglang python tests/run_bt_encoder_host.py
Uses the real LDAC encoder and ESP-IDF header; AAC/SBC and FreeRTOS are test doubles.
"""
import os
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH") or Path.home() / ".platformio/packages/framework-espidf")
STUBS = {
    "sdkconfig.h": """
#define CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 1
#define CONFIG_BT_A2DP_CODEC_AAC_ENABLED 1
#define CONFIG_BT_A2DP_SEP_NUM_MAX 3
""",
    "esp_err.h": """
#pragma once
using esp_err_t = int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
""",
    "esp_log.h": """
#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""",
    "esp_timer.h": """
#pragma once
#include <cstdint>
int64_t esp_timer_get_time();
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <cstdint>
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
""",
    "freertos/semphr.h": """
#pragma once
#include "FreeRTOS.h"
struct HostSemaphore;
using SemaphoreHandle_t = HostSemaphore*;
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
""",
    "freertos/task.h": """
#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*entry)(void*), const char*, uint32_t, void*, unsigned, void*);
int xTaskCreatePinnedToCore(void (*entry)(void*), const char*, uint32_t, void*, unsigned, void*, int);
void vTaskDelete(void*);
void vTaskDelay(TickType_t);
""",
    "host_codec.h": """
#pragma once
#include <cstdint>
using esp_audio_err_t = int;
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_ERR_FAIL = -1;
struct esp_audio_enc_in_frame_t { uint8_t* buffer; uint32_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; uint32_t len; uint32_t encoded_bytes; uint64_t pts; };
struct esp_audio_enc_info_t { uint32_t bitrate; };
struct esp_aac_enc_config_t { int sample_rate, channel, bits_per_sample, bitrate; bool adts_used; };
enum { ESP_SBC_CH_MODE_MONO, ESP_SBC_CH_MODE_DUAL, ESP_SBC_CH_MODE_STEREO, ESP_SBC_CH_MODE_JOINT_STEREO };
constexpr int ESP_SBC_ALLOC_LOUDNESS = 0;
struct esp_sbc_enc_config_t { int sample_rate, ch_mode, allocation_method, bitpool, block_length, sub_bands_num; };
#define ESP_SBC_STD_ENC_CONFIG_DEFAULT() {}
#define DECLARE_CODEC(n) \\
int esp_##n##_enc_open(void*, uint32_t, void**); \\
int esp_##n##_enc_get_frame_size(void*, int*, int*); \\
int esp_##n##_enc_get_info(void*, esp_audio_enc_info_t*); \\
int esp_##n##_enc_reset(void*); \\
int esp_##n##_enc_process(void*, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t*); \\
void esp_##n##_enc_close(void*);
DECLARE_CODEC(aac)
DECLARE_CODEC(sbc)
""",
    "esp_aac_enc.h": '#include "host_codec.h"\n',
    "esp_sbc_enc.h": '#include "host_codec.h"\n',
}

with tempfile.TemporaryDirectory(prefix="bt-encoder-") as temp:
    work = Path(temp)
    spec = importlib.util.spec_from_file_location("overlay", ROOT / "scripts/prepare_ldac_transport.py")
    overlay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(overlay)
    overlay.generate(IDF, work / "overlay")
    for name, body in STUBS.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(body)
    executable = work / ("encoder_test.exe" if os.name == "nt" else "encoder_test")
    ldac = ROOT / "components/libldac"
    objects = []
    for source in ("ldaclib.c", "ldacBT.c"):
        obj = work / (source + ".o")
        subprocess.run([sys.executable, "-m", "ziglang", "cc", "-O2", "-std=c99", "-fsigned-char",
                        "-I", str(ldac / "inc"), "-I", str(ldac / "src"),
                        "-c", str(ldac / "src" / source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run([
        sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-pthread",
        "-I", str(work), "-I", str(ROOT / "src"),
        "-I", str(work / "overlay/include"), "-I", str(ROOT / "scripts"),
        "-I", str(ldac / "inc"), "-DAVDT_CODEC_SIZE=11",
        "-I", str(IDF / "components/bt/host/bluedroid/api/include/api"),
        str(ROOT / "src/bt_encoder.cpp"), str(ROOT / "tests/bt_encoder_host.cpp"),
        *objects,
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
