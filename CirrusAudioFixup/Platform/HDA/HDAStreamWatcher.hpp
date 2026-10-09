//
// HDAStreamWatcher.hpp
// HDA stream-format decoding and the playback format gate.
// The gate accepts the clock and slot timing used by the current CS35L41
// profile. Decoding another rate does not make that rate safe for playback.
// See LICENSE for distribution terms.
//

#pragma once

#include <stdint.h>

namespace cirrus::platform::hda {

namespace registers {
constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kStreamControl = 0x00;
constexpr uint32_t kStreamStatus = 0x03;
}

class HDAStreamWatcher {
public:
    static bool isFormatSupported(uint16_t format) {
        // Bit 15 selects non-PCM; bit 7 is reserved. Channels are encoded as
        // count minus one. Compare the rate ratio exactly to avoid rounding
        // an unsupported format into the fixed 48 kHz clock profile.
        const uint32_t baseRate = (format & 0x4000) ? 44100 : 48000;
        const uint32_t multiplier = ((format >> 11) & 7) + 1;
        const uint32_t divisor = ((format >> 8) & 7) + 1;
        const uint32_t sampleSize = (format >> 4) & 7;
        return !(format & 0x8080) && multiplier <= 4 && baseRate * multiplier == 48000 * divisor &&
               (format & 15) == 1 && sampleSize >= 1 && sampleSize <= 3;
    }

    static uint32_t decodeSampleRate(uint16_t format) {
        uint32_t baseRate = (format & 0x4000) ? 44100 : 48000;
        uint8_t mult = (format >> 11) & 0x07;
        uint8_t div = (format >> 8) & 0x07;
        uint32_t rate = baseRate * (mult + 1) / (div + 1);
        return rate;
    }

    static uint8_t decodeBitsPerSample(uint16_t format) {
        switch ((format >> 4) & 0x07) {
        case 0:
            return 8;
        case 1:
            return 16;
        case 2:
            return 20;
        case 3:
            return 24;
        case 4:
            return 32;
        default:
            return 0;
        }
    }
};

}
