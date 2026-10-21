// Common register-level device operations.
// Silicon drivers provide these operations. AmplifierLifecycleHooks owns
// firmware, playback verification and the complete initialization sequence.
// See LICENSE for distribution terms.
#pragma once

#include "Core/AudioTypes.hpp"
#include "Core/BusEndpoint.hpp"
#include "Core/RegisterIO.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

class AmplifierDevice {
public:
    virtual ~AmplifierDevice() = default;

    virtual const char* name() const = 0;
    // Legacy I2C identity; new code uses endpoint(). SPI has no I2C address.
    virtual uint8_t address() const { return 0xff; }
    virtual BusEndpoint endpoint() const { return {}; }

    const char* getName() const { return name(); }
    uint8_t getAddress() const { return address(); }

    virtual bool probe(RegisterIO& io) = 0;

    enum class InitResult { Success, DeviceNotFound, ResetFailed, BootTimeout, BootError };

    virtual InitResult initialize(RegisterIO& io) = 0;

    // Firmware formats and DSP boot belong to AmplifierLifecycleHooks.
    // Register helpers must not impose WMFW/BIN on another amplifier.

    virtual bool startPlayback(RegisterIO& io) = 0;

    virtual bool stopPlayback(RegisterIO& io) = 0;

    virtual bool powerDown(RegisterIO& io) = 0;

    virtual bool isAlive(RegisterIO& io) = 0;
};

}
