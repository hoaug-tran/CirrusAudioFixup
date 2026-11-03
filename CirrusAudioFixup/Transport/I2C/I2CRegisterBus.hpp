// Register-map transactions over a combined addressed I2C controller.
#pragma once
#include "Transport/I2C/I2CController.hpp"
namespace cirrus::transport {
class I2CRegisterBus final : public core::RegisterBus {
public:
    I2CRegisterBus(I2CController* controller, I2CEndpoint endpoint) : mController(controller), mEndpoint(endpoint) {}
    ~I2CRegisterBus() override = default;
    size_t maxPayloadBytes(size_t header, bool read) const override {
        if (!mController || !validEndpoint())
            return 0;
        size_t limit = mController->maxWriteBytes();
        if (limit < header)
            return 0;
        return read ? mController->maxReadBytes() : limit - header;
    }
    IOReturn command(const uint8_t* tx, size_t nt, uint8_t* rx, size_t nr) override {
        if (!validEndpoint())
            return kIOReturnBadArgument;
        if (!mController)
            return kIOReturnNotReady;
        if (!tx || !nt || (nr && !rx) || nt > mController->maxWriteBytes() || nr > mController->maxReadBytes())
            return kIOReturnBadArgument;
        return mController->transfer(mEndpoint, tx, nt, rx, nr);
    }
    IOReturn writeSequence(const uint8_t* frames, size_t frameBytes, size_t count, uint32_t delayUs) override {
        constexpr uint32_t maxSequenceDelayUs = 100000;
        if (!validEndpoint())
            return kIOReturnBadArgument;
        if (!mController)
            return kIOReturnNotReady;
        constexpr size_t maxSequenceFrames = 256;
        if (!frames || !frameBytes || !count || frameBytes > mController->maxWriteBytes() ||
            count > maxSequenceFrames || uint64_t(count) * delayUs > maxSequenceDelayUs)
            return kIOReturnBadArgument;
        IOReturn result = kIOReturnSuccess;
        for (size_t i = 0; i < count; ++i) {
            const IOReturn transferResult = mController->transfer(mEndpoint, frames + i * frameBytes, frameBytes, nullptr, 0);
            if (result == kIOReturnSuccess)
                result = transferResult;
            if (delayUs) {
#if defined(KERNEL)
                if (delayUs < 1000)
                    IODelay(delayUs);
                else
#endif
                    IOSleep(delayUs / 1000 + (delayUs % 1000 != 0));
            }
        }
        return result;
    }

private:
    bool validEndpoint() const { return mEndpoint.address <= (mEndpoint.tenBit ? 0x3ff : 0x7f); }
    I2CController* mController;
    I2CEndpoint mEndpoint;
};
}
