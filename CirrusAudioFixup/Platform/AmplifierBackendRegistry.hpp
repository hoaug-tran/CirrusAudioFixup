// Statically linked backend definitions; no runtime executable/plugin loading.
// Definitions and context assets outlive created instances. The creator owns
// the instance and calls its matching destroy callback after joining all users.
#pragma once
#include "Core/AmplifierLifecycle.hpp"
#include "Core/BusEndpoint.hpp"
#include "Core/RegisterMap.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include "Platform/BoardConfiguration.hpp"
#include <string.h>
namespace cirrus::platform {
class AmplifierHost;
struct AmplifierBackendContext {
    core::RegisterIO& io;
    core::BusEndpoint endpoint;
    const BoardConfiguration& board;
    BoardIdentity actualBoard;
    size_t bindingIndex;
    const firmware::FirmwareBundle& firmware;
    const char* actualDeviceHid{nullptr};
    void* platformContext{nullptr};
    // Optional backend-specific bridge, negotiated by its definition. New
    // independent backends use host/io/board/firmware without a service cast.
    const char* platformContract{nullptr};
    AmplifierHost* host{nullptr};
};
struct AmplifierBackendDefinition {
    const char* name;
    const char* acpiHid;
    uint32_t siliconId;
    uint8_t transportMask; // Bit 0 I2C, bit 1 SPI. No implicit SoundWire support.
    core::RegisterMapConfig i2cFormat;
    core::RegisterMapConfig spiFormat;
    core::AmplifierLifecycleHooks* (*create)(const AmplifierBackendContext&);
    void (*destroy)(core::AmplifierLifecycleHooks*);
    bool supportsDeferredFirmware{false};
    uint32_t identityRegister{0}, identityMask{0xffffffffU};
    // Optional diagnostic/legacy model tag. Registration plus read-only identity
    // determines discovery; a central descriptor enum must not admit/reject it.
    core::AmplifierModel model{core::AmplifierModel::Unknown};
    const char* platformContract{nullptr};
    void (*inspect)(const AmplifierBackendContext&){nullptr};
    bool (*validDebugPhase)(const char*){nullptr};
    const core::RegisterMapConfig* format(core::BusTransportType bus) const {
        if (bus == core::BusTransportType::I2C && (transportMask & 1))
            return &i2cFormat;
        if (bus == core::BusTransportType::SPI && (transportMask & 2))
            return &spiFormat;
        return nullptr;
    }
};
class AmplifierBackendRegistry {
public:
    static constexpr size_t kCapacity = 32;
    size_t count() const { return mCount; }
    const AmplifierBackendDefinition* at(size_t i) const { return i < mCount ? mDefinitions[i] : nullptr; }
    bool add(const AmplifierBackendDefinition& definition) {
        if (mCount == kCapacity || !definition.name || !*definition.name || !definition.acpiHid || !*definition.acpiHid ||
            !definition.siliconId || !definition.transportMask || (definition.transportMask & ~3U) ||
            (definition.platformContract && !*definition.platformContract) || !definition.identityMask || !definition.create ||
            !definition.destroy ||
            ((definition.transportMask & 1) && (!core::RegisterMap::isValidConfig(definition.i2cFormat) || !definition.i2cFormat.readable ||
                                                !definition.i2cFormat.writable)) ||
            ((definition.transportMask & 2) &&
             (!core::RegisterMap::isValidConfig(definition.spiFormat) || !definition.spiFormat.readable || !definition.spiFormat.writable)))
            return false;
        for (size_t i = 0; i < mCount; ++i) {
            const auto& old = *mDefinitions[i];
            if (!strcmp(old.name, definition.name) || (old.siliconId == definition.siliconId && !strcmp(old.acpiHid, definition.acpiHid) &&
                                                       (old.transportMask & definition.transportMask)))
                return false;
        }
        mDefinitions[mCount++] = &definition;
        return true;
    }
    const AmplifierBackendDefinition* find(const char* hid, uint32_t siliconId, core::BusTransportType bus) const {
        if (!hid)
            return nullptr;
        for (size_t i = 0; i < mCount; ++i) {
            const auto& d = *mDefinitions[i];
            if (d.siliconId == siliconId && !strcmp(d.acpiHid, hid) && d.format(bus))
                return &d;
        }
        return nullptr;
    }
    const AmplifierBackendDefinition* findModel(const char* hid, core::AmplifierModel model, core::BusTransportType bus) const {
        const AmplifierBackendDefinition* found = nullptr;
        for (size_t i = 0; i < mCount; ++i) {
            const auto* d = mDefinitions[i];
            if (hid && !strcmp(d->acpiHid, hid) && d->model == model && d->format(bus)) {
                if (found)
                    return nullptr;
                found = d;
            }
        }
        return found;
    }
    // Factory selection does not authorize power writes. The returned backend's
    // identify/authorizeBoard hooks still run before lifecycle initialization.
    core::AmplifierLifecycleHooks* create(const AmplifierBackendContext& context, const AmplifierBackendDefinition*& selected) const {
        if (!context.board.permits(context.actualBoard) || context.bindingIndex >= context.board.count ||
            !context.endpoint.sameDevice(context.board.amplifiers[context.bindingIndex].endpoint))
            return nullptr;
        const auto& binding = context.board.amplifiers[context.bindingIndex];
        if (context.endpoint.transport == core::BusTransportType::SPI &&
            (context.endpoint.spiMode != binding.endpoint.spiMode || context.endpoint.spiClockHz > binding.endpoint.spiClockHz))
            return nullptr;
        firmware::FirmwareTarget expected{binding.siliconId,
                                          context.actualBoard.subsystemVendor,
                                          context.actualBoard.subsystemDevice,
                                          binding.speakerId,
                                          binding.channelIndex,
                                          binding.tuningResource};
        if (!context.firmware.matches(expected))
            return nullptr;
        const char* deviceHid = binding.acpiHid ? binding.acpiHid : context.actualBoard.acpiHid;
        if (binding.acpiHid && (!context.actualDeviceHid || strcmp(deviceHid, context.actualDeviceHid)))
            return nullptr;
        if (context.actualDeviceHid && strcmp(deviceHid, context.actualDeviceHid))
            return nullptr;
        const auto* definition = find(deviceHid, binding.siliconId, context.endpoint.transport);
        if (definition && definition->platformContract &&
            (!context.platformContext || !context.platformContract || strcmp(definition->platformContract, context.platformContract)))
            return nullptr;
        if (!definition || (context.firmware.format == firmware::FirmwareFormat::Deferred && !definition->supportsDeferredFirmware))
            return nullptr;
        auto* instance = definition->create(context);
        if (instance)
            selected = definition;
        return instance;
    }

private:
    const AmplifierBackendDefinition* mDefinitions[kCapacity]{};
    size_t mCount{0};
};
}
