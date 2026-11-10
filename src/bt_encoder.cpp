#include "bt_encoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "audio_pipe.h"
#include "aacenc_lib.h"
#include "esp_sbc_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "ldacBT.h"
#include "repo_ldac_transport.h"

#if !CONFIG_BT_A2DP_USE_EXTERNAL_CODEC || !CONFIG_BT_A2DP_CODEC_AAC_ENABLED
#error "Enable external A2DP codecs and AAC in sdkconfig (see sdkconfig.defaults)"
#endif
static_assert(ESP_A2D_MAX_SEPS >= 3, "LDAC/AAC/SBC output requires three A2DP endpoints");

namespace {
constexpr const char* TAG = "bt_encoder";
// Target average AAC bitrate. FDK's peak-bitrate limit (set from the MTU in open_aac) keeps every
// frame inside one media packet, which Espressif's encoder could not: it only bounds the average,
// and dense music made frames up to 1035 bytes against a JBL PartyBox's 882-byte MTU.
constexpr uint32_t AAC_BITRATE = 256000;
// Bluedroid reserves 20 bytes of each AAC media packet for the LATM header it adds.
constexpr int AAC_LATM_RESERVE = 20;
// FDK documents that frames may vary by about a byte around the peak; keep two bytes spare.
constexpr int AAC_PEAK_MARGIN = 2;
constexpr int AAC_FRAME_SAMPLES = 1024;
}

esp_err_t BtEncoder::register_endpoints(bool ldac_enabled) {
    // Lowest endpoint index wins when we initiate negotiation. Without LDAC, AAC and SBC take
    // SEIDs 0 and 1 so the registered endpoints stay contiguous, as in ESP-IDF's examples.
    esp_err_t err = ESP_OK;
    uint8_t seid = 0;
    if (ldac_enabled) {
        esp_a2d_mcc_t ldac{};
        ldac.type = ESP_A2D_MCT_NON_A2DP;
        const uint8_t rate = AudioPipe::sample_rate() == 48000 ? REPO_LDAC_RATE_48000 : REPO_LDAC_RATE_44100;
        const uint8_t ldac_cie[8] = {0x2d, 0x01, 0, 0, 0xaa, 0, rate, REPO_LDAC_STEREO};
        memcpy(ldac.cie.vendor_info, ldac_cie, sizeof(ldac_cie));
        err = esp_a2d_source_register_stream_endpoint(seid++, &ldac);
        if (err != ESP_OK) return err;
    }
    esp_a2d_mcc_t aac{};
    aac.type = ESP_A2D_MCT_M24;
    auto& a = aac.cie.m24_info;
    a.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC | ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    a.samp_freq1 = AudioPipe::sample_rate() == 44100 ? ESP_A2D_M24_CIE_SF1_44K : 0;
    a.samp_freq2 = AudioPipe::sample_rate() == 48000 ? ESP_A2D_M24_CIE_SF2_48K : 0;
    a.ch = ESP_A2D_M24_CIE_CH_1 | ESP_A2D_M24_CIE_CH_2;
    // CBR raw AAC; Bluedroid wraps it in LATM. Do not advertise unsupported rates.
    a.br1 = (AAC_BITRATE >> 16) & ESP_A2D_M24_CIE_BR1_MSK;
    a.br2 = (AAC_BITRATE >> 8) & ESP_A2D_M24_CIE_BR2_MSK;
    a.br3 = AAC_BITRATE & ESP_A2D_M24_CIE_BR3_MSK;
    err = esp_a2d_source_register_stream_endpoint(seid++, &aac);
    if (err != ESP_OK) return err;

    esp_a2d_mcc_t sbc{};
    sbc.type = ESP_A2D_MCT_SBC;
    auto& s = sbc.cie.sbc_info;
    s.samp_freq = AudioPipe::sample_rate() == 48000 ? ESP_A2D_SBC_CIE_SF_48K : ESP_A2D_SBC_CIE_SF_44K;
    s.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO | ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    s.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    s.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    s.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    s.min_bitpool = 2;
    s.max_bitpool = 53;
    return esp_a2d_source_register_stream_endpoint(seid, &sbc);
}

bool BtEncoder::configure(const esp_a2d_mcc_t& codec) {
    clear();
    esp_audio_err_t err = ESP_AUDIO_ERR_FAIL;
    aac_ = codec.type == ESP_A2D_MCT_M24;
    ldac_ = codec.type == ESP_A2D_MCT_NON_A2DP;
    pcm_bits_ = ldac_ ? AudioPipe::bits_per_sample() : 16;
    if (ldac_) {
        const uint8_t expected_rate = AudioPipe::sample_rate() == 48000 ? REPO_LDAC_RATE_48000 : REPO_LDAC_RATE_44100;
        if (!repo_ldac_cie_valid(codec.cie.vendor_info, true) || codec.cie.vendor_info[6] != expected_rate) return false;
        channels_ = 2;
        encoder_ = ldacBT_get_handle();
        input_size_ = LDACBT_ENC_LSU * AudioPipe::bytes_per_frame();
        output_size_ = LDACBT_MAX_NBYTES;
        err = encoder_ ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
    } else if (aac_) {
        const auto& a = codec.cie.m24_info;
        const bool rate_matches = AudioPipe::sample_rate() == 44100 ?
            a.samp_freq1 == ESP_A2D_M24_CIE_SF1_44K && a.samp_freq2 == 0 :
            a.samp_freq1 == 0 && a.samp_freq2 == ESP_A2D_M24_CIE_SF2_48K;
        if (!rate_matches ||
            (a.ch != ESP_A2D_M24_CIE_CH_1 && a.ch != ESP_A2D_M24_CIE_CH_2) ||
            (a.obj_type != ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC &&
             a.obj_type != ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC)) return false;
        channels_ = a.ch == ESP_A2D_M24_CIE_CH_1 ? 1 : 2;
        const uint32_t bitrate = (static_cast<uint32_t>(a.br1 & ESP_A2D_M24_CIE_BR1_MSK) << 16) |
                                 (static_cast<uint32_t>(a.br2) << 8) | a.br3;
        aac_max_bitrate_ = bitrate ? std::min(bitrate, AAC_BITRATE) : AAC_BITRATE;
        err = open_aac(0) ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
    } else if (codec.type == ESP_A2D_MCT_SBC) {
        const auto& s = codec.cie.sbc_info;
        const auto expected_rate = AudioPipe::sample_rate() == 48000 ? ESP_A2D_SBC_CIE_SF_48K : ESP_A2D_SBC_CIE_SF_44K;
        if (s.samp_freq != expected_rate || s.block_len != ESP_A2D_SBC_CIE_BLOCK_LEN_16 ||
            s.num_subbands != ESP_A2D_SBC_CIE_NUM_SUBBANDS_8 ||
            s.alloc_mthd != ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS ||
            s.min_bitpool < 2 || s.max_bitpool < s.min_bitpool || s.max_bitpool > 53) return false;
        esp_sbc_enc_config_t cfg = ESP_SBC_STD_ENC_CONFIG_DEFAULT();
        switch (s.ch_mode) {
            case ESP_A2D_SBC_CIE_CH_MODE_MONO: cfg.ch_mode = ESP_SBC_CH_MODE_MONO; break;
            case ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL: cfg.ch_mode = ESP_SBC_CH_MODE_DUAL; break;
            case ESP_A2D_SBC_CIE_CH_MODE_STEREO: cfg.ch_mode = ESP_SBC_CH_MODE_STEREO; break;
            case ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO: cfg.ch_mode = ESP_SBC_CH_MODE_JOINT_STEREO; break;
            default: return false;
        }
        channels_ = cfg.ch_mode == ESP_SBC_CH_MODE_MONO ? 1 : 2;
        cfg.sample_rate = AudioPipe::sample_rate();
        cfg.allocation_method = ESP_SBC_ALLOC_LOUDNESS;
        cfg.bitpool = s.max_bitpool;
        cfg.block_length = 16;
        cfg.sub_bands_num = 8;
        err = esp_sbc_enc_open(&cfg, sizeof(cfg), &encoder_);
        if (err == ESP_AUDIO_ERR_OK) err = esp_sbc_enc_get_frame_size(encoder_, &input_size_, &output_size_);
    }
    if (err != ESP_AUDIO_ERR_OK || input_size_ <= 0 || output_size_ <= 0) {
        ESP_LOGE(TAG, "Cannot open negotiated encoder: %d", err);
        close();
        return false;
    }
    if (!ldac_ && !aac_) {
        esp_audio_enc_info_t info{};
        if (esp_sbc_enc_get_info(encoder_, &info) == ESP_AUDIO_ERR_OK) bitrate_bps_ = info.bitrate;
    }
    const unsigned max_frames = ldac_ ? 6 : aac_ ? 1 : 7;
    pcm_ = static_cast<uint8_t*>(malloc(input_size_ * max_frames));
    encoded_ = static_cast<uint8_t*>(malloc(output_size_ * (ldac_ ? 1 : max_frames)));
    if (!pcm_ || !encoded_) {
        close();
        return false;
    }
    ESP_LOGI(TAG, "Negotiated %s, %lu Hz, %u-bit, %d channel(s)", codec_name(),
             static_cast<unsigned long>(AudioPipe::sample_rate()), pcm_bits_, channels_);
    return true;
}

// Opens FDK for the negotiated AAC configuration. With mtu 0 it only validates the configuration
// and sizes the buffers; start() reopens it for the connection's MTU, which also drops any PCM
// still delayed inside the encoder.
bool BtEncoder::open_aac(uint16_t mtu) {
    if (encoder_) {
        auto handle = static_cast<HANDLE_AACENCODER>(encoder_);
        aacEncClose(&handle);
        encoder_ = nullptr;
    }
    HANDLE_AACENCODER handle = nullptr;
    if (aacEncOpen(&handle, 0x01 /* AAC core only: no SBR or MPEG Surround */, channels_) != AACENC_OK) {
        return false;
    }
    encoder_ = handle;
    uint32_t bitrate = aac_max_bitrate_;
    uint32_t peak = 0;
    if (mtu) {
        if (mtu <= AAC_LATM_RESERVE + AAC_PEAK_MARGIN) return false;
        // Peak bits per frame are the packet payload; FDK takes the limit in bit/s.
        peak = static_cast<uint32_t>(static_cast<uint64_t>(mtu - AAC_LATM_RESERVE - AAC_PEAK_MARGIN) * 8 *
                                     AudioPipe::sample_rate() / AAC_FRAME_SAMPLES);
        bitrate = std::min(bitrate, peak);
    }
    const struct { AACENC_PARAM param; UINT value; } params[] = {
        {AACENC_AOT, AOT_AAC_LC},
        {AACENC_SAMPLERATE, AudioPipe::sample_rate()},
        {AACENC_CHANNELMODE, static_cast<UINT>(channels_ == 1 ? MODE_1 : MODE_2)},
        {AACENC_CHANNELORDER, 1},  // WAV order: left, right
        {AACENC_GRANULE_LENGTH, AAC_FRAME_SAMPLES},
        {AACENC_BITRATEMODE, 0},  // CBR, as advertised
        {AACENC_BITRATE, bitrate},
        {AACENC_TRANSMUX, TT_MP4_RAW},  // Bluedroid adds LATM
        {AACENC_AFTERBURNER, aac_afterburner_ ? 1U : 0U},
    };
    for (const auto& p : params) {
        if (aacEncoder_SetParam(handle, p.param, p.value) != AACENC_OK) return false;
    }
    if (peak && aacEncoder_SetParam(handle, AACENC_PEAK_BITRATE, peak) != AACENC_OK) return false;
    AACENC_InfoStruct info{};
    if (aacEncEncode(handle, nullptr, nullptr, nullptr, nullptr) != AACENC_OK ||
        aacEncInfo(handle, &info) != AACENC_OK || info.frameLength != AAC_FRAME_SAMPLES) return false;
    // The buffers are allocated once in configure(); a reopen must not need more.
    if (!mtu) output_size_ = static_cast<int>(info.maxOutBufBytes);
    else if (static_cast<int>(info.maxOutBufBytes) > output_size_) return false;
    input_size_ = AAC_FRAME_SAMPLES * channels_ * 2;
    bitrate_bps_ = aacEncoder_GetParam(handle, AACENC_BITRATE);
    encoder_delay_us_ = AudioPipe::frames_to_us(info.nDelay);
    return true;
}

const char* BtEncoder::codec_name() const {
    return encoder_ ? (ldac_ ? "LDAC" : aac_ ? "AAC" : "SBC") : "";
}

void BtEncoder::set_ldac_quality(LdacQuality quality) {
    // OFF only changes endpoint registration at boot; it is not an encoder mode.
    if (quality == LdacQuality::OFF || ldac_quality_ == quality) return;
    if (ldac_) stop();
    ldac_quality_ = quality;
    if (ldac_) bitrate_bps_ = 0;
}

void BtEncoder::set_aac_afterburner(bool enabled) {
    if (aac_afterburner_ == enabled) return;
    if (aac_) stop();
    aac_afterburner_ = enabled;
}

BtEncoder::Metrics BtEncoder::metrics() const {
    return {encode_avg_us_.load(), encode_max_us_.load(), late_batches_.load(),
            send_retries_.load(), dropped_packets_.load()};
}

bool BtEncoder::start(esp_a2d_conn_hdl_t connection, uint16_t mtu, ReadPcm read, Played played) {
    if (task_active_) return true;
    if (!encoder_ || !connection || mtu <= 20) return false;
    if (!stopped_) stopped_ = xSemaphoreCreateBinary();
    if (!stopped_) return false;
    connection_ = connection;
    mtu_ = mtu;
    packet_frames_ = ldac_ ? (ldac_quality_ == LdacQuality::HIGH ? 2 :
                             ldac_quality_ == LdacQuality::STANDARD ? 3 : 6) :
                     aac_ ? 1 : std::min<unsigned>(7, (mtu - 1) / output_size_);
    if (!packet_frames_) return false;
    read_ = std::move(read);
    played_ = std::move(played);
    // Resume without retaining delayed PCM in the encoder.
    if (ldac_) {
        auto handle = static_cast<HANDLE_LDAC_BT>(encoder_);
        ldacBT_close_handle(handle);
        // Sony's 679-byte transport budget reserves 18 bytes internally, yielding
        // at most 660 codec bytes at HQ/SQ/MQ. Our media header needs one more byte.
        const int mode = ldac_quality_ == LdacQuality::STANDARD ? LDACBT_EQMID_SQ :
                         ldac_quality_ == LdacQuality::CONNECTION ? LDACBT_EQMID_MQ : LDACBT_EQMID_HQ;
        if (mtu < 661 || ldacBT_init_handle_encode(handle, 679, mode,
                LDACBT_CHANNEL_MODE_STEREO, pcm_bits_ == 24 ? LDACBT_SMPL_FMT_S24 : LDACBT_SMPL_FMT_S16,
                AudioPipe::sample_rate()) != 0) {
            ESP_LOGE(TAG, "Cannot initialize LDAC for MTU %u (error 0x%x)", mtu, ldacBT_get_error_code(handle));
            return false;
        }
        bitrate_bps_ = ldacBT_get_bitrate(handle) * 1000U;
        ESP_LOGI(TAG, "LDAC: %d kbit/s, %lu Hz, %u-bit stereo, MTU %u", ldacBT_get_bitrate(handle),
                 static_cast<unsigned long>(AudioPipe::sample_rate()), pcm_bits_, mtu);
    } else if (aac_) {
        if (!open_aac(mtu)) {
            ESP_LOGE(TAG, "Cannot initialize AAC for MTU %u", mtu);
            return false;
        }
        ESP_LOGI(TAG, "AAC: %lu bit/s, frames capped at %d bytes (MTU %u), encoder delay %lld us, afterburner %s",
                 static_cast<unsigned long>(bitrate_bps_), mtu - AAC_LATM_RESERVE - AAC_PEAK_MARGIN, mtu,
                 static_cast<long long>(encoder_delay_us_), aac_afterburner_ ? "on" : "off");
    } else if (esp_sbc_enc_reset(encoder_) != ESP_AUDIO_ERR_OK) {
        return false;
    }
    running_.store(true);
    encode_avg_us_ = encode_max_us_ = late_batches_ = send_retries_ = dropped_packets_ = 0;
#if CONFIG_FREERTOS_UNICORE
    constexpr int encoder_core = 0;
#else
    constexpr int encoder_core = 1;
#endif
    // FPU use pins a task on first use. Choose core 1 explicitly so encoding
    // cannot accidentally become pinned beside Wi-Fi and the Bluetooth host.
    // FDK keeps several 4 KB scratch arrays on the stack.
    const uint32_t stack = aac_ ? 20480 : 8192;
    if (xTaskCreatePinnedToCore(task_entry, "bt_encode", stack, this, 10, nullptr, encoder_core) != pdPASS) {
        running_.store(false);
        return false;
    }
    task_active_ = true;
    return true;
}

void BtEncoder::stop() {
    if (!task_active_) return;
    running_.store(false);
    // Worker has bounded waits. Join before touching encoder, buffers or callbacks.
    xSemaphoreTake(stopped_, portMAX_DELAY);
    task_active_ = false;
}

void BtEncoder::close() {
    if (encoder_) {
        if (ldac_) ldacBT_free_handle(static_cast<HANDLE_LDAC_BT>(encoder_));
        else if (aac_) {
            auto handle = static_cast<HANDLE_AACENCODER>(encoder_);
            aacEncClose(&handle);
        }
        else esp_sbc_enc_close(encoder_);
    }
    encoder_ = nullptr;
    bitrate_bps_ = 0;
    encoder_delay_us_ = 0;
    free(pcm_);
    free(encoded_);
    pcm_ = encoded_ = nullptr;
}

void BtEncoder::clear() {
    stop();
    close();
    timestamp_ = 0;
}

void BtEncoder::task_entry(void* arg) {
    auto* self = static_cast<BtEncoder*>(arg);
    self->stream();
    ESP_LOGI(TAG, "%s encoder task stopped; stack headroom %u bytes", self->codec_name(),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    xSemaphoreGive(self->stopped_);
    vTaskDelete(nullptr);
}

void BtEncoder::stream() {
    const uint32_t samples = input_size_ * packet_frames_ / (channels_ * (pcm_bits_ / 8));
    const int64_t frame_us = AudioPipe::frames_to_us(samples);
    int64_t deadline = esp_timer_get_time();
    uint64_t frame_index = 0;
    const int64_t epoch = deadline;
    unsigned pending_count = 0;
    uint64_t encode_total_us = 0;
    uint32_t encode_batches = 0;
    while (running_.load()) {
        const int64_t now = esp_timer_get_time();
        if (now < deadline) {
            vTaskDelay(pdMS_TO_TICKS(std::max<int64_t>(1, (deadline - now + 999) / 1000)));
            continue;
        }
        // Use cumulative samples to avoid rounding drift, and never burst after a stall.
        if (now - deadline > frame_us) {
            ++late_batches_;
            frame_index = static_cast<uint64_t>(now - epoch) * AudioPipe::sample_rate() / (samples * 1000000ULL);
        }
        deadline = epoch + static_cast<int64_t>((++frame_index * samples * 1000000ULL) / AudioPipe::sample_rate());
        uint32_t real_frames = read_(pcm_, input_size_ * packet_frames_);
        // Notify at the PCM handoff using its sample-clock deadline. Waiting for
        // a later encoded packet made clock feedback sensitive to batching and
        // lost packets, even though that PCM had already left the pipe.
        // PCM entering the AAC encoder leaves it encoder_delay_us_ later.
        if (real_frames && played_) played_(real_frames,
            deadline - AudioPipe::frames_to_us(samples - real_frames) + encoder_delay_us_);
        const int64_t encode_start = esp_timer_get_time();
        esp_audio_enc_in_frame_t in{};
        in.buffer = pcm_;
        in.len = input_size_ * packet_frames_;
        esp_audio_enc_out_frame_t out{};
        out.buffer = encoded_;
        out.len = output_size_ * (ldac_ ? 1 : packet_frames_);
        unsigned frames_in_packet = packet_frames_;
        int err;
        if (ldac_) {
            frames_in_packet = 0;
            err = ESP_AUDIO_ERR_OK;
            // Process a whole transport packet per wake-up, not one 2.9 ms
            // codec frame. Sony still receives its required 128-sample inputs.
            for (unsigned chunk = 0; chunk < packet_frames_; ++chunk) {
                if (pending_count == 32) { err = ESP_AUDIO_ERR_FAIL; break; }
                ++pending_count;
                int used = 0, bytes = 0, frames = 0;
                err = ldacBT_encode(static_cast<HANDLE_LDAC_BT>(encoder_), pcm_ + chunk * input_size_,
                                    &used, encoded_, &bytes, &frames);
                if (err || used != input_size_ || bytes < 0 || bytes > output_size_ || frames < 0 ||
                    frames > 15 || static_cast<unsigned>(frames) > pending_count ||
                    ((bytes == 0) != (frames == 0)) || (bytes && out.encoded_bytes)) {
                    err = ESP_AUDIO_ERR_FAIL;
                    break;
                }
                if (bytes) {
                    out.encoded_bytes = bytes;
                    frames_in_packet = frames;
                    pending_count -= frames;
                }
            }
        } else if (aac_) {
            void* in_buffer = in.buffer;
            void* out_buffer = out.buffer;
            INT in_id = IN_AUDIO_DATA, in_bytes = static_cast<INT>(in.len), in_width = 2;
            INT out_id = OUT_BITSTREAM_DATA, out_bytes = static_cast<INT>(out.len), out_width = 1;
            AACENC_BufDesc in_desc{1, &in_buffer, &in_id, &in_bytes, &in_width};
            AACENC_BufDesc out_desc{1, &out_buffer, &out_id, &out_bytes, &out_width};
            AACENC_InArgs args{};
            args.numInSamples = static_cast<INT>(in.len / 2);
            AACENC_OutArgs result{};
            err = aacEncEncode(static_cast<HANDLE_AACENCODER>(encoder_), &in_desc, &out_desc, &args, &result) ==
                          AACENC_OK && result.numInSamples == args.numInSamples && result.numOutBytes >= 0
                      ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
            out.encoded_bytes = err == ESP_AUDIO_ERR_OK ? static_cast<uint32_t>(result.numOutBytes) : 0;
        } else {
            err = esp_sbc_enc_process(encoder_, &in, &out);
        }
        const uint32_t encode_us = static_cast<uint32_t>(esp_timer_get_time() - encode_start);
        encode_total_us += encode_us;
        encode_avg_us_.store(static_cast<uint32_t>(encode_total_us / ++encode_batches));
        if (encode_us > encode_max_us_.load()) encode_max_us_.store(encode_us);
        const uint32_t timestamp = timestamp_;
        const uint32_t packet_samples = ldac_ ? frames_in_packet * LDACBT_ENC_LSU : samples;
        // FDK's peak bitrate should keep every AAC frame inside one media packet. Bluedroid cannot
        // fragment AAC, so if one is ever larger, skip that 23 ms frame and keep the RTP clock
        // moving instead of tearing down the stream.
        if (aac_ && err == ESP_AUDIO_ERR_OK && out.encoded_bytes > static_cast<uint32_t>(mtu_ - 20)) {
            ESP_LOGW(TAG, "Skipped an AAC frame larger than the MTU: %lu bytes (MTU %u)",
                     static_cast<unsigned long>(out.encoded_bytes), mtu_);
            ++dropped_packets_;
            if (running_.load()) timestamp_ += packet_samples;  // same rule as a send below
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK ||
            out.encoded_bytes > static_cast<uint32_t>(mtu_ - (aac_ ? 20 : 1))) {
            ESP_LOGE(TAG, "Encode/MTU failure: %d, %lu bytes (MTU %u)", err,
                     static_cast<unsigned long>(out.encoded_bytes), mtu_);
            break;
        }
        if (out.encoded_bytes == 0) continue;  // encoder priming
        esp_err_t sent = ESP_FAIL;
        for (unsigned retry = 0; retry < 10 && running_.load(); ++retry) {
            auto* packet = esp_a2d_audio_buff_alloc(out.encoded_bytes);
            if (!packet) {
                sent = ESP_ERR_NO_MEM;
                break;
            }
            packet->data_len = out.encoded_bytes;
            packet->number_frame = frames_in_packet;
            packet->timestamp = timestamp;
            memcpy(packet->data, encoded_, out.encoded_bytes);
            sent = esp_a2d_source_audio_data_send(connection_, packet);
            // The SDK overlays packet metadata even on enqueue failure. Rebuild each retry.
            if (sent != ESP_OK) esp_a2d_audio_buff_free(packet);
            if (sent != ESP_FAIL) break;
            ++send_retries_;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        // A stop during encoding must not advance RTP past a packet we never
        // submitted. Congestion drops during playback still consume clock time.
        if (sent == ESP_OK || running_.load()) timestamp_ += packet_samples;
        if (sent != ESP_OK) ++dropped_packets_;
        if (sent != ESP_OK && sent != ESP_FAIL) {
            // A deliberate stop races the last packet (INVALID_STATE); only report real failures.
            if (running_.load()) {
                ESP_LOGE(TAG, "%s packet send failed: %s, %lu bytes (MTU %u)", codec_name(),
                         esp_err_to_name(sent), static_cast<unsigned long>(out.encoded_bytes), mtu_);
            }
            break;
        }
    }
    running_.store(false);
}
