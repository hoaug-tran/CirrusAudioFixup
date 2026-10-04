#pragma once

#include "Core/RegisterIO.hpp"
#include "Core/Types.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

// Abstract interface representing a smart boosted amplifier IC (CS35L41, CS35L45, CS35L51, CS35L56)
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual const char* name() const = 0;
    virtual uint8_t address() const = 0;

    // Backward-compatibility accessors
    const char* getName() const { return name(); }
    uint8_t getAddress() const { return address(); }

    // Reads device ID and silicon revision registers to confirm physical presence
    virtual bool probe(RegisterIO& io) = 0;

    enum class InitResult {
        Success,
        DeviceNotFound,
        ResetFailed,
        BootTimeout,
        BootError
    };

    // Issues software reset, waits for OTP boot, and applies silicon errata patches
    virtual InitResult initialize(RegisterIO& io) = 0;

    // Uploads Halo DSP firmware image and tuning coefficient binary into DSP SRAM
    virtual bool bootDsp(RegisterIO& io, const uint8_t* wmfw, size_t wmfwSize, const uint8_t* bin, size_t binSize) = 0;

    // Unmutes output stage and engages class D boost converter
    virtual bool startPlayback(RegisterIO& io) = 0;

    // Quiesces audio stream and safely mutes amplifier output
    virtual bool stopPlayback(RegisterIO& io) = 0;

    // Powers down charge pump and puts analog blocks into low-power sleep
    virtual bool powerDown(RegisterIO& io) = 0;

    // Polls DSP heartbeat register to verify firmware execution
    virtual bool isAlive(RegisterIO& io) = 0;
};

} // namespace cirrus::core
