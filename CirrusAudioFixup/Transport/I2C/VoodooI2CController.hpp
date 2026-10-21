// Adapt the established custom VoodooI2C addressed ABI to a generic controller.
// No native Intel/AMD I2C register layout or undocumented 10-bit ABI is assumed.
#pragma once
#include "I2CController.hpp"
#include "VoodooI2CProvider.hpp"
namespace cirrus::transport {
class VoodooI2CController final : public I2CController {
public:
    explicit VoodooI2CController(IOService* provider) : mProvider(provider) {}
    size_t maxWriteBytes() const override { return 252; }
    size_t maxReadBytes() const override { return 252; }
    IOReturn transfer(const I2CEndpoint& ep, const uint8_t* tx, size_t nt, uint8_t* rx, size_t nr) override {
        if (ep.tenBit || ep.address > 0x7f || !tx || !nt || nt > maxWriteBytes() || nr > maxReadBytes() || (nr && !rx))
            return kIOReturnBadArgument;
        // The legacy ABI borrows mutable TX memory. Never cast away the caller's const.
        uint8_t outgoing[252]{};
        for (size_t i = 0; i < nt; ++i)
            outgoing[i] = tx[i];
        VoodooI2CProvider provider(mProvider, uint8_t(ep.address));
        provider.transfer(outgoing, uint16_t(nt), rx, uint16_t(nr));
        return provider.lastReturn();
    }

private:
    IOService* mProvider;
};
}
