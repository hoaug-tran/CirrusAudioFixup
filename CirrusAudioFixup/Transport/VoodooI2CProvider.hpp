// Custom VoodooI2C addressed provider ABI. This adapter dispatches a combined
// transaction; register width, endian and silicon rules belong to RegisterMap.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

#ifndef VOODOO_I2C_TRANSFER_TO_ADDRESS
#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"
#endif

struct VoodooI2CAddressedTransfer {
    // Layout must match the provider's callPlatformFunction implementation.
    // Buffers are borrowed for the synchronous call and must remain valid.
    uint8_t address;
    uint8_t* writeBuffer;
    uint16_t writeLength;
    uint8_t* readBuffer;
    uint16_t readLength;
};

namespace cirrus::transport {
class VoodooI2CProvider final {
public:
    VoodooI2CProvider(IOService* provider,uint8_t targetAddress):mProvider(provider),mTargetAddress(targetAddress) {}
    IOReturn lastReturn() const {return mLastReturn;}
    bool transfer(uint8_t* writeBuf, uint16_t writeLen, uint8_t* readBuf, uint16_t readLen) {
        if (mTargetAddress > 0x7F || (writeLen && !writeBuf) || (readLen && !readBuf) || (!writeLen && !readLen)) {
            mLastReturn = kIOReturnBadArgument;
            return false;
        }
        if (!mProvider) {
            mLastReturn = kIOReturnNotReady;
            return false;
        }

        VoodooI2CAddressedTransfer request{};
        request.address = mTargetAddress;
        request.writeBuffer = writeBuf;
        request.writeLength = writeLen;
        request.readBuffer = readBuf;
        request.readLength = readLen;

        mLastReturn = mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS, true, &request, nullptr, nullptr, nullptr);
        return mLastReturn == kIOReturnSuccess;
    }

private:
    IOService* mProvider{nullptr}; // Borrowed through the synchronous call.
    uint8_t mTargetAddress{0};
    IOReturn mLastReturn{kIOReturnSuccess};
};
}
