// Combined addressed I2C contract. A controller owns repeated-START timing,
// endpoint speed, bus arbitration and bounded completion; CPU identity is irrelevant.
#pragma once
#include "Core/RegisterBus.hpp"
namespace cirrus::transport {
struct I2CEndpoint { uint16_t address; bool tenBit{false}; };
class I2CController {
public:
    virtual ~I2CController()=default;
    virtual size_t maxWriteBytes() const=0;
    virtual size_t maxReadBytes() const=0;
    virtual IOReturn transfer(const I2CEndpoint&,const uint8_t*,size_t,uint8_t*,size_t)=0;
};
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
