// CS35L41 SPI register framing against a synchronous controller interface.
// Protocol reference: Linux v6.12 cs35l41-lib.c (Cirrus Logic, 2017-2021)
// and drivers/base/regmap/regmap-spi.c: 32-bit BE registers/values, 16 pad
// bits, read flag 0x80 in the first address byte, one CS assertion per frame.
// This adapter does not discover controllers, reset amps or authorize boards.
#pragma once

#include "Core/RegisterIO.hpp"
#include "Transport/SPIController.hpp"

namespace cirrus::transport {

class CS35L41SPITransport final : public core::RegisterIO {
public:
    static constexpr size_t kHeaderBytes = 6;
    static constexpr size_t kMaxPayloadBytes = 256;

    // Controller is borrowed; caller owns lifetime and serializes register RMW
    // operations. Writes require explicit approval after board validation.
    CS35L41SPITransport(SPIController* controller, SPIEndpoint endpoint, bool allowWrites = false)
        : mController(controller), mEndpoint(endpoint), mAllowWrites(allowWrites) {}

    IOReturn lastReturn() const { return mLastReturn; }

    bool read(uint32_t reg, uint32_t* value) override {
        if (!value)
            return fail(kIOReturnBadArgument);
        uint8_t bytes[4]{};
        if (!bulkRead(reg, bytes, sizeof(bytes)))
            return false;
        *value = decode(bytes);
        return true;
    }

    bool write(uint32_t reg, uint32_t value) override {
        uint8_t bytes[4];
        encode(bytes, value);
        return bulkWrite(reg, bytes, sizeof(bytes));
    }

    bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) override {
        uint32_t current = 0;
        if (!read(reg, &current))
            return false;
        uint32_t next = (current & ~mask) | (value & mask);
        return next == current || write(reg, next);
    }

    bool bulkRead(uint32_t reg, uint8_t* data, size_t length) override {
        return transfer(reg, nullptr, data, length);
    }

    bool bulkWrite(uint32_t reg, const uint8_t* data, size_t length) override {
        return transfer(reg, data, nullptr, length);
    }

    bool pollBit(uint32_t reg, uint32_t mask, uint32_t target, uint32_t timeoutMs) override {
        if (!timeoutMs || (target & ~mask))
            return fail(kIOReturnBadArgument);
        for (uint32_t attempt = 0; attempt < timeoutMs; ++attempt) {
            uint32_t value = 0;
            if (!read(reg, &value))
                return false;
            if ((value & mask) == target)
                return true;
            if (attempt + 1 < timeoutMs)
                IOSleep(1);
        }
        // Budget bounds attempts, like the I2C adapter; controller exchange
        // time is additional and must itself be bounded by its implementation.
        return fail(kIOReturnTimeout);
    }

private:
    static void encode(uint8_t* out, uint32_t value) {
        for (size_t i = 0; i < 4; ++i)
            out[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
    }

    static uint32_t decode(const uint8_t* in) {
        return (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16) |
               (uint32_t(in[2]) << 8) | uint32_t(in[3]);
    }

    bool fail(IOReturn result) {
        mLastReturn = result;
        return false;
    }

    bool transfer(uint32_t reg, const uint8_t* writeData, uint8_t* readData, size_t length) {
        // Validate the whole range before any bus operation. Bit 31 is the
        // SPI read flag; CS35L41 registers and payloads have four-byte stride.
        if ((!writeData && !readData) || !length || (length & 3) || (reg & 3) ||
            reg >= 0x80000000U || length > size_t(0x80000000U - reg) ||
            !mEndpoint.clockHz || mEndpoint.mode > 3)
            return fail(kIOReturnBadArgument);
        if (writeData && !mAllowWrites)
            return fail(kIOReturnNotPermitted);
        if (!mController)
            return fail(kIOReturnNotReady);
        size_t limit = mController->maxTransferBytes();
        if (limit < kHeaderBytes + 4)
            return fail(kIOReturnNotReady);
        size_t payloadLimit = (limit - kHeaderBytes) & ~size_t(3);
        if (payloadLimit > kMaxPayloadBytes)
            payloadLimit = kMaxPayloadBytes;

        // Fixed buffers avoid allocation while uploading firmware. Each chunk
        // is a new complete register transaction; partial bulk failure leaves
        // preceding chunks transferred. Do not blindly retry power writes.
        for (size_t offset = 0; offset < length;) {
            size_t chunk = length - offset;
            if (chunk > payloadLimit)
                chunk = payloadLimit;
            uint8_t tx[kHeaderBytes + kMaxPayloadBytes]{};
            uint8_t rx[kHeaderBytes + kMaxPayloadBytes]{};
            encode(tx, reg + static_cast<uint32_t>(offset));
            if (readData)
                tx[0] |= 0x80;
            else
                for (size_t i = 0; i < chunk; ++i)
                    tx[kHeaderBytes + i] = writeData[offset + i];
            mLastReturn = mController->exchange(mEndpoint, tx, readData ? rx : nullptr, kHeaderBytes + chunk);
            if (mLastReturn != kIOReturnSuccess)
                return false;
            if (readData)
                for (size_t i = 0; i < chunk; ++i)
                    readData[offset + i] = rx[kHeaderBytes + i];
            offset += chunk;
        }
        return true;
    }

    SPIController* mController;
    SPIEndpoint mEndpoint;
    bool mAllowWrites;
    IOReturn mLastReturn{kIOReturnSuccess};
};

}
