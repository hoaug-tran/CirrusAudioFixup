// CS35L41's register format configured over the shared register-map engine.
// Linux cs35l41-lib.c: 32-bit BE address/value, 16 pad bits, read flag 0x80.
#pragma once
#include "Core/RegisterMap.hpp"
#include "SPIRegisterBus.hpp"
namespace cirrus::transport {
class CS35L41SPITransport final : public core::RegisterIO {
public:
    static constexpr size_t kHeaderBytes=6;
    static constexpr size_t kMaxPayloadBytes=256;
    CS35L41SPITransport(SPIController* controller,SPIEndpoint endpoint,bool allowWrites=false,
                       core::RegisterOperationLock* lock=nullptr)
        : mBus(controller,endpoint),mMap(mBus,config(),allowWrites,lock) {}
    IOReturn lastReturn() const { return mMap.lastReturn(); }
    int32_t lastError() const override { return mMap.lastError(); }
    bool read(uint32_t reg,uint32_t* value) override {return mMap.read(reg,value);}
    bool write(uint32_t reg,uint32_t value) override {return mMap.write(reg,value);}
    bool updateBits(uint32_t reg,uint32_t mask,uint32_t value) override {return mMap.updateBits(reg,mask,value);}
    bool bulkRead(uint32_t reg,uint8_t* data,size_t bytes) override {return mMap.bulkRead(reg,data,bytes);}
    bool bulkWrite(uint32_t reg,const uint8_t* data,size_t bytes) override {return mMap.bulkWrite(reg,data,bytes);}
    bool pollBit(uint32_t reg,uint32_t mask,uint32_t value,uint32_t attempts) override {return mMap.pollBit(reg,mask,value,attempts);}
public:
    static core::RegisterMapConfig config() {
        core::RegisterMapConfig c{}; c.paddingBytes=2; c.maxRegister=0x7ffffffc; c.readFlag=0x80; return c;
    }
private:
    SPIRegisterBus mBus;
    core::RegisterMap mMap;
};
}
