// Bounded configurable register IO. Inspired by Linux regmap configuration
// concepts; no cache/replay of power, mailbox or clear-on-read registers.
// Bulk bytes retain device transfer order; scalar values use host byte order.
#pragma once
#include "RegisterIO.hpp"
#include "RegisterBus.hpp"
namespace cirrus::core {
enum class RegisterEndian { Big, Little };
struct RegisterMapConfig {
    uint8_t addressBytes{4};
    uint8_t valueBytes{4};
    uint8_t paddingBytes{0};
    RegisterEndian addressEndian{RegisterEndian::Big};
    RegisterEndian valueEndian{RegisterEndian::Big};
    uint32_t stride{4};
    uint32_t maxRegister{0xfffffffcU};
    uint8_t readFlag{0};
    uint8_t writeFlag{0};
    bool (*readable)(uint32_t){nullptr};
    bool (*writable)(uint32_t){nullptr};
    bool (*precious)(uint32_t){nullptr};
    bool (*noIncrementReadable)(uint32_t){nullptr};
    bool (*noIncrementWritable)(uint32_t){nullptr};
};
class RegisterMap : public RegisterIO {
public:
    static constexpr size_t kFrameBytes = 262;
    static constexpr size_t kPayloadBytes = 256;
    RegisterMap(RegisterBus& bus, RegisterMapConfig config, bool allowWrites = false,
                RegisterOperationLock* lock = nullptr)
        : mBus(bus), mConfig(config), mAllowWrites(allowWrites), mLock(lock) {}
    static bool isValidConfig(const RegisterMapConfig& config) {
        return config.addressBytes>=1 && config.addressBytes<=4 &&
            config.valueBytes>=1 && config.valueBytes<=4 && config.paddingBytes<=4 &&
            config.stride && !(config.maxRegister % config.stride) &&
            (config.addressBytes==4 || config.maxRegister < (uint32_t(1)<<(config.addressBytes*8))) &&
            (config.addressEndian==RegisterEndian::Big || config.addressEndian==RegisterEndian::Little) &&
            (config.valueEndian==RegisterEndian::Big || config.valueEndian==RegisterEndian::Little);
    }
    IOReturn lastReturn() const { return mLastReturn; }
    int32_t lastError() const override { return mLastReturn; }
    bool read(uint32_t reg,uint32_t* value) override {
        Guard guard(mLock); return readUnlocked(reg,value);
    }
    bool readForDiagnostic(uint32_t reg,uint32_t* value) {
        Guard guard(mLock);
        if (mConfig.precious && mConfig.precious(reg)) return fail(kIOReturnNotPermitted);
        return readUnlocked(reg,value);
    }
    bool write(uint32_t reg,uint32_t value) override {
        Guard guard(mLock); return writeUnlocked(reg,value);
    }
    bool updateBits(uint32_t reg,uint32_t mask,uint32_t value) override {
        Guard guard(mLock);
        if (!validConfig() || (mask & ~valueMask())) return fail(kIOReturnBadArgument);
        if(mConfig.precious && mConfig.precious(reg))return fail(kIOReturnNotPermitted);
        uint32_t current=0;
        if (!readUnlocked(reg,&current)) return false;
        uint32_t next=(current & ~mask)|(value & mask);
        return next==current || writeUnlocked(reg,next);
    }
    bool bulkRead(uint32_t reg,uint8_t* data,size_t bytes) override {
        Guard guard(mLock); return transfer(reg,nullptr,data,bytes);
    }
    bool bulkWrite(uint32_t reg,const uint8_t* data,size_t bytes) override {
        Guard guard(mLock); return transfer(reg,data,nullptr,bytes);
    }
    bool noIncrementRead(uint32_t reg,uint8_t* data,size_t bytes) {
        Guard guard(mLock);return transfer(reg,nullptr,data,bytes,false);
    }
    bool noIncrementWrite(uint32_t reg,const uint8_t* data,size_t bytes) {
        Guard guard(mLock);return transfer(reg,data,nullptr,bytes,false);
    }
    bool pollBit(uint32_t reg,uint32_t mask,uint32_t target,uint32_t attempts) override {
        if (!validConfig() || !attempts || (target & ~mask) || (mask & ~valueMask())) return fail(kIOReturnBadArgument);
        for (uint32_t i=0;i<attempts;++i) {
            uint32_t value=0;
            if (!read(reg,&value)) return false;
            if ((value & mask)==target) return true;
            if (i+1<attempts) IOSleep(1);
        }
        return fail(kIOReturnTimeout);
    }
private:
    class Guard {
    public:
        explicit Guard(RegisterOperationLock* lock):mLock(lock) { if(mLock)mLock->lock(); }
        ~Guard() { if(mLock)mLock->unlock(); }
    private: RegisterOperationLock* mLock;
    };
    bool validConfig() const { return isValidConfig(mConfig); }
    uint32_t valueMask() const {
        return mConfig.valueBytes==4 ? 0xffffffffU : ((uint32_t(1)<<(mConfig.valueBytes*8))-1);
    }
    static void encode(uint8_t* bytes,uint32_t value,size_t width,RegisterEndian endian) {
        for(size_t i=0;i<width;++i) bytes[i]=uint8_t(value>>(8*(endian==RegisterEndian::Big ? width-i-1 : i)));
    }
    static uint32_t decode(const uint8_t* bytes,size_t width,RegisterEndian endian) {
        uint32_t value=0;
        for(size_t i=0;i<width;++i) value|=uint32_t(bytes[i])<<(8*(endian==RegisterEndian::Big ? width-i-1 : i));
        return value;
    }
    bool readUnlocked(uint32_t reg,uint32_t* value) {
        if(!value || !validConfig()) return fail(kIOReturnBadArgument);
        uint8_t bytes[4]{};
        if(!transfer(reg,nullptr,bytes,mConfig.valueBytes)) return false;
        *value=decode(bytes,mConfig.valueBytes,mConfig.valueEndian); return true;
    }
    bool writeUnlocked(uint32_t reg,uint32_t value) {
        if(!validConfig() || (value & ~valueMask())) return fail(kIOReturnBadArgument);
        uint8_t bytes[4]{}; encode(bytes,value,mConfig.valueBytes,mConfig.valueEndian);
        return transfer(reg,bytes,nullptr,mConfig.valueBytes);
    }
    bool fail(IOReturn result) { mLastReturn=result; return false; }
    bool transfer(uint32_t reg,const uint8_t* tx,uint8_t* rx,size_t bytes,bool increment=true) {
        if(!validConfig() || (!tx&&!rx) || !bytes || bytes%mConfig.valueBytes ||
            reg%mConfig.stride || reg>mConfig.maxRegister) return fail(kIOReturnBadArgument);
        size_t count=increment ? bytes/mConfig.valueBytes : 1;
        if(!increment) {
            auto permitted=rx ? mConfig.noIncrementReadable : mConfig.noIncrementWritable;
            if(!permitted || !permitted(reg))return fail(kIOReturnNotPermitted);
        }
        if(count-1 > size_t((mConfig.maxRegister-reg)/mConfig.stride)) return fail(kIOReturnBadArgument);
        if(tx && !mAllowWrites) return fail(kIOReturnNotPermitted);
        // Validate every addressed register before issuing the first chunk.
        for(size_t i=0;i<count;++i) {
            uint32_t address=reg+uint32_t(i)*mConfig.stride;
            uint8_t encoded[4]{}; encode(encoded,address,mConfig.addressBytes,mConfig.addressEndian);
            if(encoded[0] & (mConfig.readFlag|mConfig.writeFlag)) return fail(kIOReturnBadArgument);
            auto permitted=rx ? mConfig.readable : mConfig.writable;
            if(permitted && !permitted(address)) return fail(kIOReturnNotPermitted);
        }
        const size_t header=mConfig.addressBytes+mConfig.paddingBytes;
        size_t limit=mBus.maxPayloadBytes(header,rx!=nullptr);
        if(limit>kPayloadBytes) limit=kPayloadBytes;
        if(limit>kFrameBytes-header) limit=kFrameBytes-header;
        limit-=limit%mConfig.valueBytes;
        if(!limit) return fail(kIOReturnNotReady);
        for(size_t offset=0;offset<bytes;) {
            size_t chunk=bytes-offset; if(chunk>limit)chunk=limit;
            uint8_t frame[kFrameBytes]{};
            uint32_t address=reg+(increment ? uint32_t(offset/mConfig.valueBytes)*mConfig.stride : 0);
            encode(frame,address,mConfig.addressBytes,mConfig.addressEndian);
            frame[0]|=rx ? mConfig.readFlag : mConfig.writeFlag;
            if(tx) for(size_t i=0;i<chunk;++i)frame[header+i]=tx[offset+i];
            mLastReturn=mBus.command(frame,rx?header:header+chunk,rx?rx+offset:nullptr,rx?chunk:0);
            if(mLastReturn!=kIOReturnSuccess)return false;
            offset+=chunk;
        }
        return true;
    }
    RegisterBus& mBus;
    RegisterMapConfig mConfig;
    bool mAllowWrites;
    RegisterOperationLock* mLock;
    IOReturn mLastReturn{kIOReturnSuccess};
};
}
