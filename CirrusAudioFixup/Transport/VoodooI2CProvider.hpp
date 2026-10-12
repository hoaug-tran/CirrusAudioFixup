// Custom VoodooI2C addressed provider ABI. This adapter dispatches a combined
// transaction; register width, endian and silicon rules belong to RegisterMap.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <stddef.h>

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

// Binary evidence for the loaded 2.9.1 custom nub (UUID/hash recorded in
// docs/provider_abi_evidence.md). Never pack or reorder this private ABI.
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(VoodooI2CAddressedTransfer) == 40 && alignof(VoodooI2CAddressedTransfer) == 8, "custom provider request ABI");
static_assert(offsetof(VoodooI2CAddressedTransfer, address) == 0 && offsetof(VoodooI2CAddressedTransfer, writeBuffer) == 8 &&
                  offsetof(VoodooI2CAddressedTransfer, writeLength) == 16 && offsetof(VoodooI2CAddressedTransfer, readBuffer) == 24 &&
                  offsetof(VoodooI2CAddressedTransfer, readLength) == 32,
              "custom provider request field offsets");
#endif

namespace cirrus::transport {
class VoodooI2CProvider final {
public:
    VoodooI2CProvider(IOService* provider, uint8_t targetAddress) : mProvider(provider), mTargetAddress(targetAddress) {}
    IOReturn lastReturn() const { return mLastReturn; }
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
