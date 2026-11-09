#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "bt_encoder.h"
#include "host_codec.h"
#include "repo_ldac_transport.h"

using namespace std::chrono;
struct HostSemaphore { std::mutex mutex; std::condition_variable cv; bool ready = false; };
SemaphoreHandle_t xSemaphoreCreateBinary() { return new HostSemaphore; }
int xSemaphoreGive(SemaphoreHandle_t s) {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->ready = true;
    s->cv.notify_one();
    return 1;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
    std::unique_lock<std::mutex> lock(s->mutex);
    s->cv.wait(lock, [s] { return s->ready; });
    s->ready = false;
    return 1;
}
static bool create_fails;
int xTaskCreate(void (*entry)(void*), const char*, uint32_t, void* arg, unsigned, void*) {
    if (create_fails) return 0;
    std::thread([entry, arg] { entry(arg); }).detach();
    return pdPASS;
}
int xTaskCreatePinnedToCore(void (*entry)(void*), const char* name, uint32_t stack, void* arg,
                          unsigned priority, void* handle, int core) {
    assert(core == 1);
    return xTaskCreate(entry, name, stack, arg, priority, handle);
}
void vTaskDelete(void*) {}
void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(milliseconds(ms)); }
int64_t esp_timer_get_time() { return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count(); }

struct Codec { bool aac; int channels; };
static int live_codecs, opened_bitrate, opened_bitpool;
int esp_aac_enc_open(void* config, uint32_t, void** out) {
    const auto& cfg = *static_cast<esp_aac_enc_config_t*>(config);
    assert(cfg.sample_rate == 44100 && cfg.bits_per_sample == 16 && !cfg.adts_used);
    opened_bitrate = cfg.bitrate;
    *out = new Codec{true, cfg.channel};
    ++live_codecs;
    return 0;
}
int esp_sbc_enc_open(void* config, uint32_t, void** out) {
    const auto& cfg = *static_cast<esp_sbc_enc_config_t*>(config);
    assert(cfg.sample_rate == 44100 && cfg.block_length == 16 && cfg.sub_bands_num == 8);
    opened_bitpool = cfg.bitpool;
    *out = new Codec{false, cfg.ch_mode == ESP_SBC_CH_MODE_MONO ? 1 : 2};
    ++live_codecs;
    return 0;
}
int esp_aac_enc_get_frame_size(void* p, int* in, int* out) { *in = 1024 * 2 * static_cast<Codec*>(p)->channels; *out = 500; return 0; }
int esp_sbc_enc_get_frame_size(void* p, int* in, int* out) { *in = 128 * 2 * static_cast<Codec*>(p)->channels; *out = 119; return 0; }
int esp_aac_enc_get_info(void*, esp_audio_enc_info_t* info) { info->bitrate = opened_bitrate; return 0; }
int esp_sbc_enc_get_info(void*, esp_audio_enc_info_t* info) { info->bitrate = 123456; return 0; }
int esp_aac_enc_reset(void*) { return 0; }
int esp_sbc_enc_reset(void*) { return 0; }
void esp_aac_enc_close(void* p) { delete static_cast<Codec*>(p); --live_codecs; }
void esp_sbc_enc_close(void* p) { esp_aac_enc_close(p); }
static bool oversize;
int esp_aac_enc_process(void*, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t* out) {
    out->encoded_bytes = oversize ? 1001 : 400;
    memset(out->buffer, 0x5a, 400);
    return 0;
}
int esp_sbc_enc_process(void* p, esp_audio_enc_in_frame_t* in, esp_audio_enc_out_frame_t* out) {
    out->encoded_bytes = in->len / (128 * 2 * static_cast<Codec*>(p)->channels) * 119;
    assert(out->encoded_bytes <= out->len);
    memset(out->buffer, 0x5a, out->encoded_bytes);
    return 0;
}

static esp_a2d_mcc_t endpoints[3];
extern "C" esp_err_t esp_a2d_source_register_stream_endpoint(uint8_t index, const esp_a2d_mcc_t* codec) {
    assert(index < 3);
    endpoints[index] = *codec;
    return ESP_OK;
}
static int live_packets;
extern "C" esp_a2d_audio_buff_t* esp_a2d_audio_buff_alloc(uint16_t size) {
    auto* packet = static_cast<esp_a2d_audio_buff_t*>(calloc(1, sizeof(esp_a2d_audio_buff_t) + size));
    packet->data = reinterpret_cast<uint8_t*>(packet + 1);
    packet->buff_size = size;
    ++live_packets;
    return packet;
}
extern "C" void esp_a2d_audio_buff_free(esp_a2d_audio_buff_t* packet) { --live_packets; free(packet); }
static std::atomic<unsigned> sent{0};
static bool fail_once;
static uint32_t last_timestamp;
static unsigned last_frames;
static bool sending_ldac;
static unsigned ldac_frames_sent;
extern "C" esp_err_t esp_a2d_source_audio_data_send(esp_a2d_conn_hdl_t connection, esp_a2d_audio_buff_t* packet) {
    assert(connection == 1 && packet->data_len > 0);
    if (sending_ldac) {
        std::vector<uint8_t> payload(packet->data_len + 1);
        payload[0] = packet->number_frame;
        memcpy(payload.data() + 1, packet->data, packet->data_len);
        assert(repo_ldac_payload_valid(payload.data(), payload.size()));
        assert(payload.size() <= 661);
        if (sent.load()) assert(packet->timestamp == last_timestamp + last_frames * 128);
    } else {
        assert(packet->data[0] == 0x5a);
    }
    if (fail_once) {
        fail_once = false;
        // Model the SDK overlaying metadata before reporting a full queue.
        memset(packet, 0xa5, sizeof(*packet));
        return ESP_FAIL;
    }
    if (sent.load()) assert(packet->timestamp > last_timestamp);
    if (sending_ldac) ldac_frames_sent += packet->number_frame;
    last_timestamp = packet->timestamp;
    last_frames = packet->number_frame;
    esp_a2d_audio_buff_free(packet);
    ++sent;
    return ESP_OK;
}

static void wait_for_packets(unsigned count) {
    const auto deadline = steady_clock::now() + seconds(2);
    while (sent.load() < count && steady_clock::now() < deadline) std::this_thread::sleep_for(milliseconds(1));
    assert(sent.load() >= count);
}

int main() {
    uint8_t caps[11] = {10, 0, 0xff, 0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 1};
    uint8_t config[11]{};
    assert(repo_ldac_preferred_config(config, caps) && config[9] == 0x20);
    caps[9] = 0x04;  // Recognize a 96 kHz-only speaker, but fall back at 44.1 kHz.
    assert(repo_ldac_info_known(caps) && !repo_ldac_info_valid(caps, false));
    assert(!repo_ldac_config_in_caps(config, caps));
    caps[3] = 0x4f;  // Another vendor must never enter the LDAC transport.
    assert(!repo_ldac_info_known(caps));
    BtEncoder encoder;
    assert(BtEncoder::register_endpoints() == ESP_OK);
    assert(endpoints[0].type == ESP_A2D_MCT_NON_A2DP && endpoints[1].type == ESP_A2D_MCT_M24 &&
           endpoints[2].type == ESP_A2D_MCT_SBC);
    auto aac = endpoints[1];
    aac.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
    assert(encoder.configure(aac) && !encoder.mono());
    assert(opened_bitrate == 160000);
    assert(encoder.bitrate_bps() == 160000);
    auto read = [](uint8_t* pcm, size_t bytes) { memset(pcm, 0, bytes); return static_cast<uint32_t>(bytes / 4); };
    std::atomic<unsigned> played{0};
    auto report = [&played](uint32_t frames, int64_t finish) { assert(finish > 0); played += frames; };
    create_fails = true;
    assert(!encoder.start(1, 1000, read, report));
    create_fails = false;
    fail_once = true;
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(3);
    encoder.stop();
    assert(!encoder.failed() && played > 0 && last_frames == 1 && live_packets == 0);
    const unsigned before = sent.load();
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(before + 2);
    encoder.stop();

    auto sbc = endpoints[2];
    sbc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
    sbc.cie.sbc_info.max_bitpool = 31;
    assert(encoder.configure(sbc) && encoder.mono() && opened_bitpool == 31 && live_codecs == 1);
    assert(encoder.bitrate_bps() == 123456);
    sent = 0;
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(2);
    encoder.stop();
    assert(last_frames == 7 && live_packets == 0);

    sent = 0;
    assert(encoder.configure(aac));
    oversize = true;
    assert(encoder.start(1, 1000, read, report));
    const auto deadline = steady_clock::now() + seconds(2);
    while (!encoder.failed() && steady_clock::now() < deadline) std::this_thread::sleep_for(milliseconds(1));
    assert(encoder.failed());
    encoder.stop();
    assert(sent == 0 && live_packets == 0);
    // Exercise the actual Sony encoder, including buffered output and an enqueue
    // failure, against the same framing validator used by the receiver project.
    oversize = false;
    encoder.set_ldac_quality(LdacQuality::HIGH);
    assert(encoder.configure(endpoints[0]) && !encoder.mono() && live_codecs == 0);
    assert(strcmp(encoder.codec_name(), "LDAC") == 0);
    assert(!encoder.start(1, 660, read, report));
    sending_ldac = true;
    fail_once = true;
    sent = 0;
    played = 0;
    unsigned phase = 0;
    auto tone = [&phase](uint8_t* pcm, size_t bytes) {
        auto* out = reinterpret_cast<int16_t*>(pcm);
        for (size_t i = 0; i < bytes / 4; ++i) {
            out[2 * i] = (phase++ % 64) < 32 ? 6000 : -6000;
            out[2 * i + 1] = -out[2 * i];
        }
        return static_cast<uint32_t>(bytes / 4);
    };
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 909000);
    wait_for_packets(8);
    encoder.stop();
    assert(!encoder.failed() && last_frames > 1 && played >= 8 * 128 && live_packets == 0);
    assert(played == ldac_frames_sent * 128);
    const unsigned ldac_before = sent.load();
    encoder.set_ldac_quality(LdacQuality::STANDARD);
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 606000);
    wait_for_packets(ldac_before + 3);
    encoder.stop();
    assert(!encoder.failed());
    assert(played == ldac_frames_sent * 128);
    const unsigned before_connection_mode = sent.load();
    encoder.set_ldac_quality(LdacQuality::CONNECTION);
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 303000);
    wait_for_packets(before_connection_mode + 3);
    encoder.stop();
    assert(!encoder.failed() && played == ldac_frames_sent * 128);
    auto wrong_ldac = endpoints[0];
    wrong_ldac.cie.vendor_info[0] = 0;
    assert(!encoder.configure(wrong_ldac));
    aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_32K;
    assert(!encoder.configure(aac));
    encoder.clear();
    assert(encoder.bitrate_bps() == 0);
    assert(live_codecs == 0);
    puts("PASS: real LDAC encoder/framing, AAC/SBC configuration, retries, timestamps, resume, MTU rejection and cleanup");
}
