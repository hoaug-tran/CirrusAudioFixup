#pragma once

#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace cirrus::platform::hda {

// Intel High Definition Audio (HDA) MMIO stream descriptor register offsets
namespace registers {
constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kStreamControl = 0x00;
constexpr uint32_t kStreamStatus = 0x03;
constexpr uint32_t kStreamFormat = 0x12;
} // namespace registers

// Snoops Intel/AMD High Definition Audio controller MMIO registers to align stream rates with smart amps
class HDAStreamWatcher {
public:
    static bool isFormatSupported(uint16_t format) {
        if (!format)
            return false;
        // Non-PCM formats (AC-3, DTS) are not supported on direct amplifier ASP ports
        if (format & 0x8000)
            return false;
        uint8_t bits = (format >> 4) & 0x07;
        // Supported word lengths: 16-bit, 20-bit, 24-bit, 32-bit
        if (bits > 4)
            return false;
        return true;
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

} // namespace cirrus::platform::hda
