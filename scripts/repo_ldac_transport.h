#ifndef REPO_LDAC_TRANSPORT_H
#define REPO_LDAC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Adapted from esp32-s31-ldac-receiver's project-local transport.
// The SDK's NONE sentinel collides with the vendor codec's wire ID.
#define REPO_BT_CODEC_NONE 0xfe
#define REPO_LDAC_CODEC_ID 0xff
#define REPO_LDAC_INFO_LEN 11
#define REPO_LDAC_RATE_44100 0x20
#define REPO_LDAC_RATE_48000 0x10
#define REPO_LDAC_RATES (REPO_LDAC_RATE_44100 | REPO_LDAC_RATE_48000)
#define REPO_LDAC_STEREO 0x01

#if defined(ESP_PLATFORM) && (!defined(AVDT_CODEC_SIZE) || AVDT_CODEC_SIZE < REPO_LDAC_INFO_LEN)
#error "LDAC requires AVDT_CODEC_SIZE >= 11 in every Bluedroid translation unit"
#endif

static inline bool repo_ldac_cie_known(const uint8_t *cie)
{
    static const uint8_t id[6] = {0x2d, 0x01, 0, 0, 0xaa, 0};
    return memcmp(cie, id, sizeof(id)) == 0;
}

static inline bool repo_ldac_cie_valid(const uint8_t *cie, bool configuration)
{
    if (!repo_ldac_cie_known(cie)) return false;
    if (configuration)
        return (cie[6] == REPO_LDAC_RATE_44100 || cie[6] == REPO_LDAC_RATE_48000) && cie[7] == REPO_LDAC_STEREO;
    return (cie[6] & REPO_LDAC_RATES) && (cie[7] & REPO_LDAC_STEREO);
}

static inline bool repo_ldac_info_known(const uint8_t *info)
{
    return info[0] == 10 && info[1] == 0 && info[2] == REPO_LDAC_CODEC_ID &&
           repo_ldac_cie_known(info + 3);
}

static inline bool repo_ldac_info_valid(const uint8_t *info, bool configuration)
{
    return info[0] == 10 && info[1] == 0 && info[2] == REPO_LDAC_CODEC_ID &&
           repo_ldac_cie_valid(info + 3, configuration);
}

static inline bool repo_ldac_config_in_caps(const uint8_t *config, const uint8_t *caps)
{
    return repo_ldac_info_valid(config, true) && repo_ldac_info_valid(caps, false) &&
           (config[9] & caps[9]);
}

static inline bool repo_ldac_preferred_config(uint8_t *output, const uint8_t *caps)
{
    if (!repo_ldac_info_valid(caps, false)) return false;
    memcpy(output, caps, REPO_LDAC_INFO_LEN);
    output[9] = caps[9] & REPO_LDAC_RATE_44100 ? REPO_LDAC_RATE_44100 : REPO_LDAC_RATE_48000;
    output[10] = REPO_LDAC_STEREO;
    return true;
}

// LDAC's media header counts complete codec frames (no fragmentation).
static inline bool repo_ldac_payload_valid(const uint8_t *payload, size_t size)
{
    if (size < 5 || (payload[0] & 0xf0) || !(payload[0] & 0x0f)) return false;
    size_t offset = 1;
    unsigned count = 0;
    while (offset < size) {
        if (size - offset < 3 || payload[offset] != 0xaa ||
            (payload[offset + 1] >> 5) > 1 ||
            ((payload[offset + 1] >> 3) & 3) != 2) return false;
        size_t frame_size = (((size_t)(payload[offset + 1] & 7) << 6) |
                             (payload[offset + 2] >> 2)) + 4;
        if (frame_size > size - offset) return false;
        offset += frame_size;
        ++count;
    }
    return count == (payload[0] & 0x0f);
}

#endif
