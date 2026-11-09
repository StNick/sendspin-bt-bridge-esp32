#include "bt_encoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "audio_pipe.h"
#include "esp_aac_enc.h"
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
constexpr uint32_t AAC_BITRATE = 160000;
}

esp_err_t BtEncoder::register_endpoints() {
    // Lowest endpoint index wins when we initiate negotiation.
    esp_a2d_mcc_t ldac{};
    ldac.type = ESP_A2D_MCT_NON_A2DP;
    const uint8_t ldac_cie[8] = {0x2d, 0x01, 0, 0, 0xaa, 0, REPO_LDAC_RATE_44100, REPO_LDAC_STEREO};
    memcpy(ldac.cie.vendor_info, ldac_cie, sizeof(ldac_cie));
    esp_err_t err = esp_a2d_source_register_stream_endpoint(0, &ldac);
    if (err != ESP_OK) return err;
    esp_a2d_mcc_t aac{};
    aac.type = ESP_A2D_MCT_M24;
    auto& a = aac.cie.m24_info;
    a.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC | ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    a.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
    a.ch = ESP_A2D_M24_CIE_CH_1 | ESP_A2D_M24_CIE_CH_2;
    // CBR raw AAC; Bluedroid wraps it in LATM. Do not advertise unsupported rates.
    a.br1 = (AAC_BITRATE >> 16) & ESP_A2D_M24_CIE_BR1_MSK;
    a.br2 = (AAC_BITRATE >> 8) & ESP_A2D_M24_CIE_BR2_MSK;
    a.br3 = AAC_BITRATE & ESP_A2D_M24_CIE_BR3_MSK;
    err = esp_a2d_source_register_stream_endpoint(1, &aac);
    if (err != ESP_OK) return err;

    esp_a2d_mcc_t sbc{};
    sbc.type = ESP_A2D_MCT_SBC;
    auto& s = sbc.cie.sbc_info;
    s.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    s.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO | ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    s.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    s.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    s.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    s.min_bitpool = 2;
    s.max_bitpool = 53;
    return esp_a2d_source_register_stream_endpoint(2, &sbc);
}

bool BtEncoder::configure(const esp_a2d_mcc_t& codec) {
    clear();
    esp_audio_err_t err = ESP_AUDIO_ERR_FAIL;
    aac_ = codec.type == ESP_A2D_MCT_M24;
    ldac_ = codec.type == ESP_A2D_MCT_NON_A2DP;
    if (ldac_) {
        if (!repo_ldac_cie_valid(codec.cie.vendor_info, true)) return false;
        channels_ = 2;
        encoder_ = ldacBT_get_handle();
        input_size_ = LDACBT_ENC_LSU * AudioPipe::BYTES_PER_FRAME;
        output_size_ = LDACBT_MAX_NBYTES;
        err = encoder_ ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
    } else if (aac_) {
        const auto& a = codec.cie.m24_info;
        if (a.samp_freq1 != ESP_A2D_M24_CIE_SF1_44K || a.samp_freq2 != 0 ||
            (a.ch != ESP_A2D_M24_CIE_CH_1 && a.ch != ESP_A2D_M24_CIE_CH_2) ||
            (a.obj_type != ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC &&
             a.obj_type != ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC)) return false;
        channels_ = a.ch == ESP_A2D_M24_CIE_CH_1 ? 1 : 2;
        const uint32_t bitrate = (static_cast<uint32_t>(a.br1 & ESP_A2D_M24_CIE_BR1_MSK) << 16) |
                                 (static_cast<uint32_t>(a.br2) << 8) | a.br3;
        esp_aac_enc_config_t cfg{};
        cfg.sample_rate = AudioPipe::SAMPLE_RATE;
        cfg.channel = channels_;
        cfg.bits_per_sample = 16;
        cfg.bitrate = bitrate ? std::min(bitrate, AAC_BITRATE) : AAC_BITRATE;
        cfg.adts_used = false;
        err = esp_aac_enc_open(&cfg, sizeof(cfg), &encoder_);
        if (err == ESP_AUDIO_ERR_OK) err = esp_aac_enc_get_frame_size(encoder_, &input_size_, &output_size_);
    } else if (codec.type == ESP_A2D_MCT_SBC) {
        const auto& s = codec.cie.sbc_info;
        if (s.samp_freq != ESP_A2D_SBC_CIE_SF_44K || s.block_len != ESP_A2D_SBC_CIE_BLOCK_LEN_16 ||
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
        cfg.sample_rate = AudioPipe::SAMPLE_RATE;
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
    if (!ldac_) {
        esp_audio_enc_info_t info{};
        const auto got = aac_ ? esp_aac_enc_get_info(encoder_, &info) : esp_sbc_enc_get_info(encoder_, &info);
        if (got == ESP_AUDIO_ERR_OK) bitrate_bps_ = info.bitrate;
    }
    const unsigned max_frames = ldac_ ? 6 : aac_ ? 1 : 7;
    pcm_ = static_cast<uint8_t*>(malloc(input_size_ * max_frames));
    encoded_ = static_cast<uint8_t*>(malloc(output_size_ * (ldac_ ? 1 : max_frames)));
    if (!pcm_ || !encoded_) {
        close();
        return false;
    }
    ESP_LOGI(TAG, "Negotiated %s, 44100 Hz, %d channel(s)", codec_name(), channels_);
    return true;
}

const char* BtEncoder::codec_name() const {
    return encoder_ ? (ldac_ ? "LDAC" : aac_ ? "AAC" : "SBC") : "";
}

void BtEncoder::set_ldac_quality(LdacQuality quality) {
    if (ldac_quality_ == quality) return;
    if (ldac_) stop();
    ldac_quality_ = quality;
    if (ldac_) bitrate_bps_ = 0;
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
                LDACBT_CHANNEL_MODE_STEREO, LDACBT_SMPL_FMT_S16, AudioPipe::SAMPLE_RATE) != 0) {
            ESP_LOGE(TAG, "Cannot initialize LDAC for MTU %u (error 0x%x)", mtu, ldacBT_get_error_code(handle));
            return false;
        }
        bitrate_bps_ = ldacBT_get_bitrate(handle) * 1000U;
        ESP_LOGI(TAG, "LDAC: %d kbit/s, 44100 Hz stereo, MTU %u", ldacBT_get_bitrate(handle), mtu);
    } else {
        const auto err = aac_ ? esp_aac_enc_reset(encoder_) : esp_sbc_enc_reset(encoder_);
        if (err != ESP_AUDIO_ERR_OK) return false;
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
    if (xTaskCreatePinnedToCore(task_entry, "bt_encode", 8192, this, 10, nullptr, encoder_core) != pdPASS) {
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
        else if (aac_) esp_aac_enc_close(encoder_);
        else esp_sbc_enc_close(encoder_);
    }
    encoder_ = nullptr;
    bitrate_bps_ = 0;
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
    xSemaphoreGive(self->stopped_);
    vTaskDelete(nullptr);
}

void BtEncoder::stream() {
    const uint32_t samples = input_size_ * packet_frames_ / (channels_ * sizeof(int16_t));
    const int64_t frame_us = AudioPipe::frames_to_us(samples);
    int64_t deadline = esp_timer_get_time();
    uint64_t frame_index = 0;
    const int64_t epoch = deadline;
    // LDAC buffers several 128-sample inputs before producing a packet. Keep
    // real/filler accounting until those exact frames are handed to A2DP.
    uint32_t pending_real[32]{};
    unsigned pending_head = 0, pending_count = 0;
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
            frame_index = static_cast<uint64_t>(now - epoch) * AudioPipe::SAMPLE_RATE / (samples * 1000000ULL);
        }
        deadline = epoch + static_cast<int64_t>((++frame_index * samples * 1000000ULL) / AudioPipe::SAMPLE_RATE);
        uint32_t real_frames = read_(pcm_, input_size_ * packet_frames_);
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
            uint32_t unread_real = real_frames;
            real_frames = 0;
            frames_in_packet = 0;
            err = ESP_AUDIO_ERR_OK;
            // Process a whole transport packet per wake-up, not one 2.9 ms
            // codec frame. Sony still receives its required 128-sample inputs.
            for (unsigned chunk = 0; chunk < packet_frames_; ++chunk) {
                if (pending_count == 32) { err = ESP_AUDIO_ERR_FAIL; break; }
                const uint32_t chunk_real = std::min<uint32_t>(unread_real, LDACBT_ENC_LSU);
                unread_real -= chunk_real;
                pending_real[(pending_head + pending_count++) % 32] = chunk_real;
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
                    for (int i = 0; i < frames; ++i) {
                        real_frames += pending_real[pending_head];
                        pending_head = (pending_head + 1) % 32;
                        --pending_count;
                    }
                }
            }
        } else {
            err = aac_ ? esp_aac_enc_process(encoder_, &in, &out) : esp_sbc_enc_process(encoder_, &in, &out);
        }
        const uint32_t encode_us = static_cast<uint32_t>(esp_timer_get_time() - encode_start);
        encode_total_us += encode_us;
        encode_avg_us_.store(static_cast<uint32_t>(encode_total_us / ++encode_batches));
        if (encode_us > encode_max_us_.load()) encode_max_us_.store(encode_us);
        const uint32_t timestamp = timestamp_;
        const uint32_t packet_samples = ldac_ ? frames_in_packet * LDACBT_ENC_LSU : samples;
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
        if (sent == ESP_OK && real_frames && played_) played_(real_frames, now + AudioPipe::frames_to_us(packet_samples));
        if (sent != ESP_OK && sent != ESP_FAIL) break;
    }
    running_.store(false);
}
