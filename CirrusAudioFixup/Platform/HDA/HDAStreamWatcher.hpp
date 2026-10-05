#pragma once

#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

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
        if (!format)
            return false;

        if (format & 0x8000)
            return false;
        uint8_t bits = (format >> 4) & 0x07;

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

}
