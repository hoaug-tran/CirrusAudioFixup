// Borrowed SPI endpoint mapped to the common register transaction interface.
#pragma once
#include "Core/RegisterBus.hpp"
#include "SPIController.hpp"
namespace cirrus::transport {
class SPIRegisterBus final : public core::RegisterBus {
public:
    SPIRegisterBus(SPIController* controller, SPIEndpoint endpoint) : mController(controller), mEndpoint(endpoint) {}
    size_t maxPayloadBytes(size_t header, bool) const override {
        if (!mController || !mEndpoint.clockHz || mEndpoint.mode > 3)
            return 0;
        size_t limit = mController->maxTransferBytes();
        return limit > header ? limit - header : 0;
    }
    IOReturn command(const uint8_t* tx, size_t nt, uint8_t* rx, size_t nr) override {
        if (!mController)
            return kIOReturnNotReady;
        if (!mEndpoint.clockHz || mEndpoint.mode > 3)
            return kIOReturnBadArgument;
        return mController->command(mEndpoint, tx, nt, rx, nr);
    }

    IOReturn writeSequence(const uint8_t* frames, size_t frameBytes, size_t count, uint32_t delayUs) override {
        if (!mController)
            return kIOReturnNotReady;
        if (!mEndpoint.clockHz || mEndpoint.mode > 3)
            return kIOReturnBadArgument;
        return mController->writeSequence(mEndpoint, frames, frameBytes, count, delayUs);
    }

private:
    SPIController* mController;
    SPIEndpoint mEndpoint;
};
}
