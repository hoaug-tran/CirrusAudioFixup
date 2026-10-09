//
// AudioDevice.hpp
// Common register-level device operations.
// This interface does not yet own the complete driver lifecycle. CS35L41 DSP
// upload, mailbox sequencing and playback verification remain in the service.
// See LICENSE for distribution terms.
//

#pragma once

#include "Core/RegisterIO.hpp"
#include "Core/Types.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual const char* name() const = 0;
    virtual uint8_t address() const = 0;

    const char* getName() const { return name(); }
    uint8_t getAddress() const { return address(); }

    virtual bool probe(RegisterIO& io) = 0;

    enum class InitResult { Success, DeviceNotFound, ResetFailed, BootTimeout, BootError };

    virtual InitResult initialize(RegisterIO& io) = 0;

    virtual bool bootDsp(RegisterIO& io, const uint8_t* wmfw, size_t wmfwSize, const uint8_t* bin, size_t binSize) = 0;

    virtual bool startPlayback(RegisterIO& io) = 0;

    virtual bool stopPlayback(RegisterIO& io) = 0;

    virtual bool powerDown(RegisterIO& io) = 0;

    virtual bool isAlive(RegisterIO& io) = 0;
};

}
