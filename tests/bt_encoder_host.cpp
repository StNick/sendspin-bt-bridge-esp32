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
#include "audio_pipe.h"
#include "host_codec.h"
#include "aacenc_lib.h"
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
static int live_codecs, opened_bitpool;
// The firmware's FDK-AAC calls land here (see run_bt_encoder_host.py) and reach the real encoder.
static std::atomic<int> oversize_frames{0};
extern "C" AACENC_ERROR host_aacEncOpen(HANDLE_AACENCODER* handle, UINT modules, UINT channels) {
    const auto err = aacEncOpen(handle, modules, channels);
    if (err == AACENC_OK) ++live_codecs;
    return err;
}
extern "C" AACENC_ERROR host_aacEncClose(HANDLE_AACENCODER* handle) {
    --live_codecs;
    return aacEncClose(handle);
}
static int last_afterburner = -1;  // value the firmware last gave FDK
extern "C" AACENC_ERROR host_aacEncoder_SetParam(HANDLE_AACENCODER handle, AACENC_PARAM param, UINT value) {
    if (param == AACENC_AFTERBURNER) last_afterburner = static_cast<int>(value);
    return aacEncoder_SetParam(handle, param, value);
}
// The next N AAC frames are inflated past any test MTU, as Espressif's encoder could do.
extern "C" AACENC_ERROR host_aacEncEncode(HANDLE_AACENCODER handle, const AACENC_BufDesc* in,
                                          const AACENC_BufDesc* out, const AACENC_InArgs* in_args,
                                          AACENC_OutArgs* out_args) {
    const auto err = aacEncEncode(handle, in, out, in_args, out_args);
    if (err == AACENC_OK && in && oversize_frames.load() > 0) {
        --oversize_frames;
        assert(out->bufSizes[0] >= 1001);
        out_args->numOutBytes = 1001;
    }
    return err;
}
int esp_sbc_enc_open(void* config, uint32_t, void** out) {
    const auto& cfg = *static_cast<esp_sbc_enc_config_t*>(config);
    assert(cfg.sample_rate == AudioPipe::sample_rate() && cfg.block_length == 16 && cfg.sub_bands_num == 8);
    opened_bitpool = cfg.bitpool;
    *out = new Codec{false, cfg.ch_mode == ESP_SBC_CH_MODE_MONO ? 1 : 2};
    ++live_codecs;
    return 0;
}
int esp_sbc_enc_get_frame_size(void* p, int* in, int* out) { *in = 128 * 2 * static_cast<Codec*>(p)->channels; *out = 119; return 0; }
int esp_sbc_enc_get_info(void*, esp_audio_enc_info_t* info) { info->bitrate = 123456; return 0; }
int esp_sbc_enc_reset(void*) { return 0; }
void esp_sbc_enc_close(void* p) { delete static_cast<Codec*>(p); --live_codecs; }
static bool sbc_oversize;
int esp_sbc_enc_process(void* p, esp_audio_enc_in_frame_t* in, esp_audio_enc_out_frame_t* out) {
    out->encoded_bytes = in->len / (128 * 2 * static_cast<Codec*>(p)->channels) * 119;
    assert(out->encoded_bytes <= out->len);
    memset(out->buffer, 0x5a, out->encoded_bytes);
    if (sbc_oversize) out->encoded_bytes = 1000;  // a packet-sizing bug; never sent
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
static uint32_t last_timestamp, first_timestamp;
static unsigned last_frames;
static bool sending_ldac;
static uint16_t aac_mtu;  // nonzero while real AAC packets are expected
static std::atomic<uint32_t> largest_aac{0};
extern "C" esp_err_t esp_a2d_source_audio_data_send(esp_a2d_conn_hdl_t connection, esp_a2d_audio_buff_t* packet) {
    assert(connection == 1 && packet->data_len > 0);
    if (sending_ldac) {
        std::vector<uint8_t> payload(packet->data_len + 1);
        payload[0] = packet->number_frame;
        memcpy(payload.data() + 1, packet->data, packet->data_len);
        assert(repo_ldac_payload_valid(payload.data(), payload.size()));
        assert(payload.size() <= 661);
        if (sent.load()) assert(packet->timestamp == last_timestamp + last_frames * 128);
    } else if (aac_mtu) {
        // Bluedroid adds up to 20 bytes of LATM; every FDK frame must leave room for it.
        assert(packet->data_len + 20 <= aac_mtu && packet->number_frame == 1);
        if (packet->data_len > largest_aac.load()) largest_aac.store(packet->data_len);
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
    else first_timestamp = packet->timestamp;
    last_timestamp = packet->timestamp;
    last_frames = packet->number_frame;
    esp_a2d_audio_buff_free(packet);
    ++sent;
    return ESP_OK;
}

static void wait_for_packets(unsigned count) {
    const auto deadline = steady_clock::now() + seconds(5);
    while (sent.load() < count && steady_clock::now() < deadline) std::this_thread::sleep_for(milliseconds(1));
    assert(sent.load() >= count);
}

static void test_pcm() {
    uint8_t packed[6]{}, output[6]{};
    for (uint8_t bits : {16, 24}) {
        for (int32_t v : {-8388608, -1536000, 0, 1536000, bits == 16 ? 8388352 : 8388607}) {
            pcm::write(packed, bits, v);
            assert(pcm::read(packed, bits) == v);
        }
    }
    pcm::write(packed, 24, 8388607);
    pcm::write(packed + 3, 24, -8388608);
    pcm::convert(output, packed, 1, 24, 16, false, 65536);
    assert(pcm::read(output, 16) == 8388352 && pcm::read(output + 2, 16) == -8388608);
    pcm::convert(output, packed, 1, 24, 16, true, 65536);
    assert(pcm::read(output, 16) == 0);
    for (int32_t v : {-8388608, -1536000, 1536000, 8388606}) {
        pcm::write(packed, 24, v);
        pcm::write(packed + 3, 24, v);
        pcm::convert(output, packed, 1, 24, 24, false, 32768);
        assert(pcm::read(output, 24) == v / 2 && pcm::read(output + 3, 24) == v / 2);
        pcm::convert(packed, packed, 1, 24, 24, false, 0);
        assert(pcm::read(packed, 24) == 0 && pcm::read(packed + 3, 24) == 0);
    }
    pcm::write(output, 16, 128); assert(pcm::read(output, 16) == 256);
    pcm::write(output, 16, -128); assert(pcm::read(output, 16) == -256);
}

int main() {
    test_pcm();
    uint8_t caps[11] = {10, 0, 0xff, 0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 1};
    uint8_t config[11]{};
    assert(repo_ldac_preferred_config(config, caps) && config[9] == 0x20);
    caps[9] = 0x10;
    assert(repo_ldac_preferred_config(config, caps) && config[9] == 0x10);
    caps[9] = 0x04;  // Recognize a 96 kHz-only speaker, but fall back at 44.1 kHz.
    assert(repo_ldac_info_known(caps) && !repo_ldac_info_valid(caps, false));
    assert(!repo_ldac_config_in_caps(config, caps));
    caps[3] = 0x4f;  // Another vendor must never enter the LDAC transport.
    assert(!repo_ldac_info_known(caps));
    BtEncoder encoder;
    // LDAC off: AAC and SBC take the first two endpoint IDs, with nothing left in the third.
    memset(endpoints, 0xee, sizeof(endpoints));  // sentinel: SBC is type 0
    assert(BtEncoder::register_endpoints(false) == ESP_OK);
    assert(endpoints[0].type == ESP_A2D_MCT_M24 && endpoints[1].type == ESP_A2D_MCT_SBC &&
           endpoints[2].type == 0xee);
    encoder.set_ldac_quality(LdacQuality::OFF);  // not an encoder mode; must be ignored
    assert(BtEncoder::register_endpoints(true) == ESP_OK);
    assert(endpoints[0].type == ESP_A2D_MCT_NON_A2DP && endpoints[1].type == ESP_A2D_MCT_M24 &&
           endpoints[2].type == ESP_A2D_MCT_SBC);
    auto aac = endpoints[1];
    aac.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
    assert(encoder.configure(aac) && !encoder.mono() && live_codecs == 1);
    assert(encoder.bitrate_bps() == 256000 && last_afterburner == 0);  // off unless enabled
    auto read = [&encoder](uint8_t* pcm, size_t bytes) {
        memset(pcm, 0, bytes);
        return static_cast<uint32_t>(bytes / ((encoder.mono() ? 1 : 2) * (encoder.pcm_bits() / 8)));
    };
    std::atomic<unsigned> played{0};
    // Shortest time between a PCM handoff and its reported playout; AAC adds FDK's encoder delay.
    std::atomic<int64_t> min_lead{INT64_MAX};
    auto report = [&played, &min_lead](uint32_t frames, int64_t finish) {
        assert(finish > 0);
        played += frames;
        const int64_t lead = finish - esp_timer_get_time();
        if (lead < min_lead.load()) min_lead.store(lead);
    };
    // Full-scale white noise is the hardest input to fit under a peak bitrate.
    uint32_t noise_state = 1;
    auto noise = [&noise_state](uint8_t* pcm, size_t bytes) {
        for (size_t i = 0; i + 1 < bytes; i += 2) {
            noise_state = noise_state * 1664525U + 1013904223U;
            const auto sample = static_cast<int16_t>(noise_state >> 16);
            pcm[i] = static_cast<uint8_t>(sample);
            pcm[i + 1] = static_cast<uint8_t>(sample >> 8);
        }
        return static_cast<uint32_t>(bytes / 4);
    };
    create_fails = true;
    assert(!encoder.start(1, 1000, read, report));
    create_fails = false;
    fail_once = true;
    aac_mtu = 1000;
    assert(encoder.start(1, 1000, read, report));
    assert(encoder.bitrate_bps() == 256000 && live_codecs == 1);  // reopened, not leaked
    wait_for_packets(3);
    encoder.stop();
    assert(!encoder.failed() && played > 0 && last_frames == 1 && live_packets == 0);
    assert(min_lead.load() >= 30000);  // FDK-AAC delays 44.1 kHz audio by about 36 ms
    const unsigned before = sent.load();
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(before + 2);
    encoder.stop();
    assert(!encoder.start(1, 22, read, report));  // no room for any AAC frame
    // The peak bitrate follows the MTU: noise at a small MTU (bitrate capped below the target)
    // and at the JBL PartyBox's 882 bytes (target kept) never needs more than one packet.
    // Both supported rates: a 48 kHz frame is shorter, so the same MTU allows a higher peak.
    for (uint32_t rate : {48000U, 44100U}) {  // ends on 44.1 kHz for the tests below
        encoder.clear();
        assert(AudioPipe::configure_format(rate, 16));
        assert(BtEncoder::register_endpoints(true) == ESP_OK);
        aac = endpoints[1];
        aac.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
        aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
        assert(encoder.configure(aac) && live_codecs == 1);
        for (uint16_t mtu : {672, 882}) {
            aac_mtu = mtu;
            largest_aac = 0;
            sent = 0;
            assert(encoder.start(1, mtu, noise, report));
            const uint32_t peak = (mtu - 22) * 8ULL * rate / 1024;
            const uint32_t expected = std::min<uint32_t>(256000, peak);
            assert(encoder.bitrate_bps() <= expected && encoder.bitrate_bps() + 1000 >= expected);
            wait_for_packets(mtu == 672 ? 80 : 40);
            encoder.stop();
            assert(!encoder.failed() && encoder.metrics().dropped_packets == 0);
            assert(largest_aac.load() + 22 <= mtu && largest_aac.load() > (mtu - 22) / 2);
        }
    }
    // Toggling the afterburner stops a running AAC encoder without marking it failed; the next
    // start (bt_app restarts it) reopens FDK with the new setting and keeps streaming.
    aac_mtu = 882;
    sent = 0;
    assert(encoder.start(1, 882, noise, report) && last_afterburner == 0);
    wait_for_packets(2);
    encoder.set_aac_afterburner(true);
    assert(!encoder.failed());
    const unsigned before_afterburner = sent.load();
    assert(encoder.start(1, 882, noise, report) && last_afterburner == 1 && live_codecs == 1);
    wait_for_packets(before_afterburner + 3);
    encoder.set_aac_afterburner(false);
    assert(encoder.start(1, 882, noise, report) && last_afterburner == 0);
    encoder.stop();
    assert(!encoder.failed() && live_packets == 0);
    aac_mtu = 0;

    auto sbc = endpoints[2];
    sbc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
    sbc.cie.sbc_info.max_bitpool = 31;
    assert(encoder.configure(sbc) && encoder.mono() && opened_bitpool == 31 && live_codecs == 1);
    assert(min_lead.exchange(INT64_MAX) >= 0);
    assert(encoder.bitrate_bps() == 123456);
    sent = 0;
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(2);
    encoder.stop();
    assert(last_frames == 7 && live_packets == 0);
    // Unlike AAC, an SBC packet over the MTU is a sizing bug: fail closed, send nothing.
    sbc_oversize = true;
    sent = 0;
    assert(encoder.start(1, 1000, read, report));
    auto deadline = steady_clock::now() + seconds(2);
    while (!encoder.failed() && steady_clock::now() < deadline) std::this_thread::sleep_for(milliseconds(1));
    assert(encoder.failed());
    encoder.stop();
    assert(sent == 0 && live_packets == 0);
    sbc_oversize = false;

    // An AAC frame larger than one media packet (the encoder's bit reservoir can do this) is
    // skipped: the stream keeps running, the PCM is still accounted for, and RTP time advances
    // by exactly the skipped frames so later audio keeps its place on the timeline.
    assert(encoder.configure(aac));
    sent = 0;
    aac_mtu = 1000;
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(1);
    encoder.stop();  // a stop never advances RTP past the last submitted packet
    const uint32_t base = last_timestamp;
    const unsigned played_before_oversize = played.load();
    const uint32_t dropped_before_oversize = encoder.metrics().dropped_packets;
    oversize_frames = 3;
    sent = 0;
    assert(encoder.start(1, 1000, read, report));
    wait_for_packets(1);
    encoder.stop();
    assert(!encoder.failed() && live_packets == 0 && oversize_frames == 0);
    assert(first_timestamp == base + 4 * 1024);
    assert(encoder.metrics().dropped_packets == dropped_before_oversize + 3);
    assert(played - played_before_oversize >= 4 * 1024);
    aac_mtu = 0;
    // Exercise the actual Sony encoder, including buffered output and an enqueue
    // failure, against the same framing validator used by the receiver project.
    encoder.set_ldac_quality(LdacQuality::HIGH);
    assert(encoder.configure(endpoints[0]) && !encoder.mono() && live_codecs == 0);
    assert(strcmp(encoder.codec_name(), "LDAC") == 0);
    assert(!encoder.start(1, 660, read, report));
    sending_ldac = true;
    fail_once = true;
    sent = 0;
    played = 0;
    unsigned phase = 0;
    unsigned consumed = 0;
    auto tone = [&phase, &consumed](uint8_t* data, size_t bytes) {
        const auto bits = AudioPipe::bits_per_sample();
        const auto width = bits / 8;
        const unsigned frames = bytes / AudioPipe::bytes_per_frame();
        for (unsigned i = 0; i < frames; ++i) {
            const int32_t v = ((phase++ % 64) < 32 ? 6000 : -6000) * 256 + (bits == 24 ? 37 : 0);
            pcm::write(data + 2 * i * width, bits, v);
            pcm::write(data + (2 * i + 1) * width, bits, -v);
        }
        consumed += frames;
        return frames;
    };
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 909000);
    wait_for_packets(8);
    encoder.stop();
    assert(!encoder.failed() && last_frames > 1 && played >= 8 * 128 && live_packets == 0);
    assert(played == consumed);
    const unsigned ldac_before = sent.load();
    encoder.set_ldac_quality(LdacQuality::STANDARD);
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 606000);
    wait_for_packets(ldac_before + 3);
    encoder.stop();
    assert(!encoder.failed());
    assert(played == consumed);
    const unsigned before_connection_mode = sent.load();
    encoder.set_ldac_quality(LdacQuality::CONNECTION);
    assert(encoder.start(1, 1000, tone, report));
    assert(encoder.bitrate_bps() == 303000);
    wait_for_packets(before_connection_mode + 3);
    encoder.stop();
    assert(!encoder.failed() && played == consumed);
    // Every packed PCM format and LDAC quality, using real Sony bitstreams.
    for (uint32_t rate : {44100U, 48000U}) {
        for (uint8_t bits : {16, 24}) {
            encoder.clear();
            assert(AudioPipe::configure_format(rate, bits));
            assert(AudioPipe::frames_to_us(rate) == 1000000);
            assert(AudioPipe::bytes_per_frame() == 2 * (bits / 8));
            assert(BtEncoder::register_endpoints(true) == ESP_OK);
            assert(endpoints[0].cie.vendor_info[6] == (rate == 44100 ? 0x20 : 0x10));
            auto fallback = endpoints[1];
            fallback.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
            fallback.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
            assert(encoder.configure(fallback) && encoder.pcm_bits() == 16);
            fallback = endpoints[2];
            fallback.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
            assert(encoder.configure(fallback) && encoder.pcm_bits() == 16);
            assert(encoder.configure(endpoints[0]) && encoder.pcm_bits() == bits);
            auto wrong_rate = endpoints[0];
            wrong_rate.cie.vendor_info[6] = rate == 44100 ? 0x10 : 0x20;
            assert(!encoder.configure(wrong_rate));
            assert(encoder.configure(endpoints[0]));
            for (auto quality : {LdacQuality::HIGH, LdacQuality::STANDARD, LdacQuality::CONNECTION}) {
                encoder.set_ldac_quality(quality);
                sent = 0; played = 0; consumed = 0;
                assert(encoder.start(1, 1000, tone, report));
                const unsigned high = rate == 44100 ? 909000 : 990000;
                const unsigned expected = quality == LdacQuality::HIGH ? high :
                    quality == LdacQuality::STANDARD ? high * 2 / 3 : high / 3;
                assert(encoder.bitrate_bps() == expected);
                wait_for_packets(4);
                encoder.stop();
                assert(!encoder.failed() && played == consumed && live_packets == 0);
            }
            // Partial reads simulate pipe starvation. Only real PCM is reported,
            // including priming and any PCM consumed before the worker stops.
            sent = 0; played = 0; consumed = 0;
            auto partial = [&consumed, bits](uint8_t* data, size_t bytes) {
                memset(data, 0, bytes);
                const unsigned real = bytes / (2 * (bits / 8)) / 2;
                consumed += real;
                return real;
            };
            assert(encoder.start(1, 1000, partial, report));
            wait_for_packets(3);
            encoder.stop();
            assert(played == consumed);
        }
    }
    assert(!AudioPipe::configure_format(96000, 24));
    assert(!AudioPipe::configure_format(44100, 32));
    encoder.clear();
    assert(AudioPipe::configure_format(44100, 16));
    auto wrong_ldac = endpoints[0];
    wrong_ldac.cie.vendor_info[0] = 0;
    assert(!encoder.configure(wrong_ldac));
    aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_32K;
    assert(!encoder.configure(aac));
    encoder.clear();
    assert(encoder.bitrate_bps() == 0);
    assert(live_codecs == 0);
    puts("PASS: 16/24-bit PCM conversion, 44.1/48 kHz LDAC across all quality modes, real LDAC encoder/framing, real FDK-AAC at 44.1/48 kHz with MTU peak caps, encoder delay and afterburner toggling, AAC/SBC configuration, retries, timestamps, resume, oversized AAC frame skipping, SBC MTU rejection and cleanup");
}
