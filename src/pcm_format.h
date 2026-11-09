#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pcm {
inline bool valid_format(uint32_t rate, uint8_t bits) {
    return (rate == 44100 || rate == 48000) && (bits == 16 || bits == 24);
}

// Signed 24-bit full-scale intermediate, independent of packed input width.
inline int32_t read(const uint8_t* p, uint8_t bits) {
    if (bits == 16) {
        const uint32_t v = p[0] | (static_cast<uint32_t>(p[1]) << 8);
        return (static_cast<int32_t>(v ^ 0x8000U) - 0x8000) * 256;
    }
    const uint32_t v = p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    return static_cast<int32_t>(v ^ 0x800000U) - 0x800000;
}

inline void write(uint8_t* p, uint8_t bits, int32_t sample) {
    sample = std::clamp<int32_t>(sample, -8388608, 8388607);
    if (bits == 16) {
        const int32_t rounded = sample >= 0 ? (sample + 128) / 256 :
            -static_cast<int32_t>((-static_cast<int64_t>(sample) + 128) / 256);
        const uint32_t v = static_cast<uint32_t>(std::clamp<int32_t>(rounded, -32768, 32767));
        p[0] = v;
        p[1] = v >> 8;
    } else {
        const uint32_t v = static_cast<uint32_t>(sample);
        p[0] = v;
        p[1] = v >> 8;
        p[2] = v >> 16;
    }
}

inline int32_t gain(int32_t sample, uint32_t q16) {
    return static_cast<int32_t>((static_cast<int64_t>(sample) * q16) / 65536);
}
// Convert packed stereo frames to the encoder's width/channel layout.
inline void convert(uint8_t* out, const uint8_t* in, size_t frames,
                    uint8_t source_bits, uint8_t output_bits, bool mono, uint32_t q16) {
    const size_t width = source_bits / 8, output_width = output_bits / 8;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t left = read(in, source_bits), right = read(in + width, source_bits);
        in += 2 * width;
        if (mono) {
            write(out, output_bits, gain((left + right) / 2, q16));
            out += output_width;
        } else {
            write(out, output_bits, gain(left, q16));
            write(out + output_width, output_bits, gain(right, q16));
            out += 2 * output_width;
        }
    }
}
}  // namespace pcm
