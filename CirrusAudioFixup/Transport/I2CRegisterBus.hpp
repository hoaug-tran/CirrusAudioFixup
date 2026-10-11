// Register-map transactions over a combined addressed I2C controller.
#pragma once
#include "Transport/I2CController.hpp"
namespace cirrus::transport {
class I2CRegisterBus final : public core::RegisterBus {
public:
    I2CRegisterBus(I2CController* controller,I2CEndpoint endpoint):mController(controller),mEndpoint(endpoint) {}
    size_t maxPayloadBytes(size_t header,bool read) const override {
        if(!mController || !validEndpoint())return 0;
        size_t limit=mController->maxWriteBytes();
        if(limit<header)return 0;
        return read ? mController->maxReadBytes() : limit-header;
    }
    IOReturn command(const uint8_t* tx,size_t nt,uint8_t* rx,size_t nr) override {
        if(!validEndpoint())return kIOReturnBadArgument;
        if(!mController)return kIOReturnNotReady;
        if(!tx || !nt || (nr&&!rx) || nt>mController->maxWriteBytes() || nr>mController->maxReadBytes())
            return kIOReturnBadArgument;
        return mController->transfer(mEndpoint,tx,nt,rx,nr);
    }
private:
    bool validEndpoint() const {return mEndpoint.address<=(mEndpoint.tenBit?0x3ff:0x7f);}
    I2CController* mController;
    I2CEndpoint mEndpoint;
};
}
