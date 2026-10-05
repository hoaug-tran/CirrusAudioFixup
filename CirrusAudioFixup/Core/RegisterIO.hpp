#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

class RegisterIO {
public:
    virtual ~RegisterIO() = default;

    virtual bool read(uint32_t reg, uint32_t* value) = 0;

    virtual bool write(uint32_t reg, uint32_t value) = 0;

    virtual bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) = 0;

    virtual bool bulkRead(uint32_t reg, uint8_t* data, size_t length) = 0;

    virtual bool bulkWrite(uint32_t reg, const uint8_t* data, size_t length) = 0;

    virtual bool pollBit(uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs) = 0;
};

}
