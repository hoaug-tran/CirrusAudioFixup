// Bus identity belongs to an endpoint, not to CPU vendor or amplifier model.
// Source paths are borrowed immutable board/ACPI data, valid for driver lifetime.
#pragma once
#include "AudioTypes.hpp"
#include <string.h>
namespace cirrus::core {
struct BusEndpoint {
    BusTransportType transport{BusTransportType::I2C};
    const char* controllerPath{nullptr};
    uint16_t i2cAddress{0};
    bool tenBitAddress{false};
    uint32_t spiChipSelect{0};
    uint32_t spiClockHz{0};
    uint8_t spiMode{0};
    bool valid() const {
        if (!controllerPath || !*controllerPath)
            return false;
        if (transport == BusTransportType::I2C)
            return i2cAddress <= (tenBitAddress ? 0x3ff : 0x7f);
        if (transport == BusTransportType::SPI)
            return spiClockHz && spiMode <= 3;
        return false; // SoundWire needs its own discovery/transaction contract.
    }
    bool sameDevice(const BusEndpoint& other) const {
        if (!valid() || !other.valid() || transport != other.transport || strcmp(controllerPath, other.controllerPath))
            return false;
        return transport == BusTransportType::I2C ? i2cAddress == other.i2cAddress && tenBitAddress == other.tenBitAddress
                                                  : spiChipSelect == other.spiChipSelect;
    }
};
}
