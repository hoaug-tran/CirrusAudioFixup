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
}
