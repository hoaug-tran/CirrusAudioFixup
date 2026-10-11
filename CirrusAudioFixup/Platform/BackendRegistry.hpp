// Statically linked backend definitions; no runtime executable/plugin loading.
// Definitions and context assets outlive created instances. The creator owns
// the instance and calls its matching destroy callback after joining all users.
#pragma once
#include "Core/AmplifierLifecycle.hpp"
#include "Core/RegisterMap.hpp"
#include "Core/Endpoint.hpp"
#include "Platform/BoardConfiguration.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include <string.h>
namespace cirrus::platform {
struct BackendContext {
    core::RegisterIO& io;
    core::BusEndpoint endpoint;
    const BoardConfiguration& board;
    BoardIdentity actualBoard;
    size_t bindingIndex;
    const firmware::FirmwareBundle& firmware;
    const char* actualDeviceHid{nullptr};
    void* platformContext{nullptr};
};
struct BackendDefinition {
    const char* name;
    const char* acpiHid;
    uint32_t siliconId;
    uint8_t transportMask; // Bit 0 I2C, bit 1 SPI. No implicit SoundWire support.
    core::RegisterMapConfig i2cFormat;
    core::RegisterMapConfig spiFormat;
    core::AmplifierLifecycleHooks* (*create)(const BackendContext&);
    void (*destroy)(core::AmplifierLifecycleHooks*);
    bool supportsDeferredFirmware{false};
    uint32_t identityRegister{0},identityMask{0xffffffffU};
    const core::RegisterMapConfig* format(core::BusTransportType bus) const {
        if(bus==core::BusTransportType::I2C && (transportMask&1))return &i2cFormat;
        if(bus==core::BusTransportType::SPI && (transportMask&2))return &spiFormat;
        return nullptr;
    }
};
class BackendRegistry {
public:
    static constexpr size_t kCapacity=32;
    bool add(const BackendDefinition& definition) {
        if(mCount==kCapacity || !definition.name || !*definition.name || !definition.acpiHid ||
            !*definition.acpiHid || !definition.siliconId || !definition.transportMask ||
            (definition.transportMask&~3U) || !definition.identityMask || !definition.create || !definition.destroy ||
            ((definition.transportMask&1) && (!core::RegisterMap::isValidConfig(definition.i2cFormat) || !definition.i2cFormat.readable || !definition.i2cFormat.writable)) ||
            ((definition.transportMask&2) && (!core::RegisterMap::isValidConfig(definition.spiFormat) || !definition.spiFormat.readable || !definition.spiFormat.writable)))return false;
        for(size_t i=0;i<mCount;++i) {
            const auto& old=*mDefinitions[i];
            if(!strcmp(old.name,definition.name) || (old.siliconId==definition.siliconId &&
                !strcmp(old.acpiHid,definition.acpiHid) && (old.transportMask&definition.transportMask)))return false;
        }
        mDefinitions[mCount++]=&definition;return true;
    }
    const BackendDefinition* find(const char* hid,uint32_t siliconId,core::BusTransportType bus) const {
        if(!hid)return nullptr;
        for(size_t i=0;i<mCount;++i) {
            const auto& d=*mDefinitions[i];
            if(d.siliconId==siliconId && !strcmp(d.acpiHid,hid) && d.format(bus))return &d;
        }
        return nullptr;
    }
    // Factory selection does not authorize power writes. The returned backend's
    // identify/authorizeBoard hooks still run before lifecycle initialization.
    core::AmplifierLifecycleHooks* create(const BackendContext& context,const BackendDefinition*& selected) const {
        if(!context.board.permits(context.actualBoard) || context.bindingIndex>=context.board.count ||
            !context.endpoint.sameDevice(context.board.amplifiers[context.bindingIndex].endpoint))return nullptr;
        const auto& binding=context.board.amplifiers[context.bindingIndex];
        if(context.endpoint.transport==core::BusTransportType::SPI &&
            (context.endpoint.spiMode!=binding.endpoint.spiMode || context.endpoint.spiClockHz>binding.endpoint.spiClockHz))return nullptr;
        firmware::FirmwareTarget expected{binding.siliconId,context.actualBoard.subsystemVendor,
            context.actualBoard.subsystemDevice,binding.speakerId,binding.channelIndex,binding.tuningResource};
        if(!context.firmware.matches(expected))return nullptr;
        const char* deviceHid=binding.acpiHid?binding.acpiHid:context.actualBoard.acpiHid;
        if(binding.acpiHid && (!context.actualDeviceHid || strcmp(deviceHid,context.actualDeviceHid)))return nullptr;
        if(context.actualDeviceHid && strcmp(deviceHid,context.actualDeviceHid))return nullptr;
        const auto* definition=find(deviceHid,binding.siliconId,context.endpoint.transport);
        if(!definition || (context.firmware.format==firmware::FirmwareFormat::Deferred && !definition->supportsDeferredFirmware))return nullptr;
        auto* instance=definition->create(context);
        if(instance)selected=definition;
        return instance;
    }
private:
    const BackendDefinition* mDefinitions[kCapacity]{};
    size_t mCount{0};
};
}
