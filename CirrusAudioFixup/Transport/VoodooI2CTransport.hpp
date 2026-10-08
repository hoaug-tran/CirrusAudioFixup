#pragma once

#include "Core/RegisterIO.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

#ifndef VOODOO_I2C_TRANSFER_TO_ADDRESS
#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"
#endif

struct VoodooI2CAddressedTransfer {
    uint8_t address;
    uint8_t* writeBuffer;
    uint16_t writeLength;
    uint8_t* readBuffer;
    uint16_t readLength;
};

namespace cirrus::transport {

class VoodooI2CTransport final : public core::RegisterIO {
public:
    static constexpr size_t kMaxChunkSize = 252;
    explicit VoodooI2CTransport(IOService* provider, uint8_t slaveAddress) : mProvider(provider), mSlaveAddress(slaveAddress) {}

    uint8_t slaveAddress() const { return mSlaveAddress; }
    IOReturn lastReturn() const { return mLastReturn; }

    bool transfer(uint8_t* writeBuf, uint16_t writeLen, uint8_t* readBuf, uint16_t readLen) {
        return dispatchTransfer(writeBuf, writeLen, readBuf, readLen);
    }

    bool read(uint32_t reg, uint32_t* value) override {
        if (!value)
            return false;
        uint8_t regBuf[4];
        writeBigEndian32(regBuf, reg);
        uint8_t valBuf[4] = {0};
        if (!dispatchTransfer(regBuf, sizeof(regBuf), valBuf, sizeof(valBuf)))
            return false;
        *value = readBigEndian32(valBuf);
        return true;
    }

    bool write(uint32_t reg, uint32_t value) override {
        uint8_t buf[8];
        writeBigEndian32(buf, reg);
        writeBigEndian32(buf + 4, value);
        return dispatchTransfer(buf, sizeof(buf), nullptr, 0);
    }

    bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) override {
        uint32_t currentVal = 0;
        if (!read(reg, &currentVal))
            return false;
        uint32_t newVal = (currentVal & ~mask) | (value & mask);
        if (newVal == currentVal)
            return true;
        return write(reg, newVal);
    }

    bool bulkRead(uint32_t reg, uint8_t* data, size_t length) override {
        if (!data || length == 0)
            return false;

        size_t offset = 0;
        while (offset < length) {
            size_t chunk = length - offset;
            if (chunk > kMaxChunkSize)
                chunk = kMaxChunkSize;
            uint8_t regBuf[4];
            writeBigEndian32(regBuf, reg + static_cast<uint32_t>(offset));
            if (!dispatchTransfer(regBuf, sizeof(regBuf), data + offset, static_cast<uint16_t>(chunk)))
                return false;
            offset += chunk;
        }
        return true;
    }

    bool bulkWrite(uint32_t reg, const uint8_t* data, size_t length) override {
        if (!data || length == 0)
            return false;
        size_t offset = 0;
        while (offset < length) {
            size_t chunk = length - offset;
            if (chunk > (kMaxChunkSize - 4))
                chunk = kMaxChunkSize - 4;
            uint8_t packet[kMaxChunkSize];
            writeBigEndian32(packet, reg + static_cast<uint32_t>(offset));
            for (size_t i = 0; i < chunk; ++i)
                packet[4 + i] = data[offset + i];
            if (!dispatchTransfer(packet, static_cast<uint16_t>(4 + chunk), nullptr, 0))
                return false;
            offset += chunk;
        }
        return true;
    }

    bool pollBit(uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs) override {
        for (uint32_t elapsed = 0; elapsed < timeoutMs; ++elapsed) {
            uint32_t val = 0;
            if (read(reg, &val) && ((val & mask) == targetVal))
                return true;
            IODelay(1000);
        }
        return false;
    }

private:
    static void writeBigEndian32(uint8_t* dst, uint32_t val) {
        dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
        dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
        dst[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
        dst[3] = static_cast<uint8_t>(val & 0xFF);
    }

    static uint32_t readBigEndian32(const uint8_t* src) {
        return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) | (static_cast<uint32_t>(src[2]) << 8) |
               static_cast<uint32_t>(src[3]);
    }

    bool dispatchTransfer(uint8_t* writeBuf, uint16_t writeLen, uint8_t* readBuf, uint16_t readLen) {
        if (mSlaveAddress > 0x7F || (writeLen && !writeBuf) || (readLen && !readBuf) || (!writeLen && !readLen)) {
            mLastReturn = kIOReturnBadArgument;
            return false;
        }
        if (!mProvider) {
            mLastReturn = kIOReturnNotReady;
            return false;
        }

        VoodooI2CAddressedTransfer request{};
        request.address = mSlaveAddress;
        request.writeBuffer = writeBuf;
        request.writeLength = writeLen;
        request.readBuffer = readBuf;
        request.readLength = readLen;

        mLastReturn = mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS, true, &request, nullptr, nullptr, nullptr);
        return mLastReturn == kIOReturnSuccess;
    }

    IOService* mProvider{nullptr};
    uint8_t mSlaveAddress{0};
    IOReturn mLastReturn{kIOReturnSuccess};
};

} // namespace cirrus::transport
