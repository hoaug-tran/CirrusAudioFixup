#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

// Hardware register bus abstraction decoupling amplifier logic from physical transport (I2C/SPI/SoundWire)
class RegisterIO {
public:
    virtual ~RegisterIO() = default;

    // Reads 32-bit register value with big-endian byte-order conversion
    virtual bool read(uint32_t reg, uint32_t* value) = 0;

    // Writes 32-bit register value
    virtual bool write(uint32_t reg, uint32_t value) = 0;

    // Atomic read-modify-write updating only the bits specified by mask
    virtual bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) = 0;

    // Multi-byte burst read across contiguous DSP memory or register space
    virtual bool bulkRead(uint32_t reg, uint8_t* data, size_t length) = 0;

    // Multi-byte burst write chunked to transport FIFO limits (e.g. 252 bytes on VoodooI2C)
    virtual bool bulkWrite(uint32_t reg, const uint8_t* data, size_t length) = 0;

    // Polls register until (reg & mask) == targetVal or timeout occurs
    virtual bool pollBit(uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs) = 0;
};

} // namespace cirrus::core
