// Interpret the Cirrus HDA _DSD contract independently of OEM, machine name,
// controller namespace path and firmware filenames. Missing data fails closed.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Devices/Amplifiers/CS35L41/DeviceConfiguration.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/Board/BoardProperties.hpp"
#include "Platform/Board/BoardDiscovery.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
using ResourceAvailable = bool (*)(uint16_t, uint16_t, uint32_t);
class CS35L41BoardDiscovery final : public platform::DiscoveredBoard {
public:
    // Current resource schema has L0/R0.
    static constexpr size_t kCapacity = 2;
    platform::AmplifierBinding bindings[kCapacity]{};
    DeviceConfiguration devices[kCapacity]{};
    char controllerPaths[kCapacity][256]{};
    char hid[32]{};
    bool resetDeclared{false};
    int32_t resetResource{-1};
    bool prepareHardware(platform::BoardDiscoveryContext::GPIOAccess& gpio) const override {
        return (!resetDeclared && resetResource < 0) || gpio.reset(resetDeclared ? "reset-gpios" : nullptr, resetResource);
    }
};
inline platform::DiscoveredBoard* resolveBoardConfiguration(const platform::BoardDiscoveryContext& context, ResourceAvailable available,
                                                               const char*& reason) {
    reason = "CS35L41_PROPERTIES_INCOMPLETE";
    if (!context.identity.acpiHid || !context.identity.subsystemVendor || !context.identity.subsystemDevice ||
        strlen(context.identity.acpiHid) >= sizeof(CS35L41BoardDiscovery::hid) || !context.endpoints || !context.count ||
        context.count > CS35L41BoardDiscovery::kCapacity || !available)
        return nullptr;
    constexpr size_t capacity = CS35L41BoardDiscovery::kCapacity;
    uint32_t indices[capacity]{}, channels[capacity]{}, boost[capacity]{}, gpio1[capacity]{}, gpio2[capacity]{};
    uint32_t inductor[capacity]{}, peak[capacity]{}, capacitor[capacity]{};
    const auto& properties = context.properties;
    struct RequiredArray {
        const char* name;
        uint32_t* values;
        const char* failure;
    };
    const RequiredArray required[] = {
        {"cirrus,dev-index", indices, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,dev-index"},
        {"cirrus,speaker-position", channels, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,speaker-position"},
        {"cirrus,boost-type", boost, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,boost-type"},
        {"cirrus,gpio1-func", gpio1, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,gpio1-func"},
        {"cirrus,gpio2-func", gpio2, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,gpio2-func"},
        {"cirrus,boost-ind-nanohenry", inductor, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,boost-ind-nanohenry"},
        {"cirrus,boost-peak-milliamp", peak, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,boost-peak-milliamp"},
        {"cirrus,boost-cap-microfarad", capacitor, "CS35L41_PROPERTIES_INCOMPLETE:cirrus,boost-cap-microfarad"},
    };
    const auto* facts = boardProperties(context.identity.subsystemVendor, context.identity.subsystemDevice);
    for (size_t fieldIndex = 0; fieldIndex < sizeof(required) / sizeof(required[0]); ++fieldIndex) {
        const auto& field = required[fieldIndex];
        if (properties.array(field.name, field.values, context.count))
            continue;
        bool external = true;
        for (size_t i = 0; i < context.count; ++i)
            external = external && boost[i] == 1;
        // External power has no internal inductor/current/capacitor settings.
        if (fieldIndex >= 5 && external && !properties.contains(field.name))
            continue;
        if (properties.contains(field.name) || !facts || facts->count != context.count ||
            context.endpoints[0].transport != core::BusTransportType::I2C) {
            reason = field.failure;
            return nullptr;
        }
        for (size_t i = 0; i < context.count; ++i) {
            if (context.endpoints[i].transport != core::BusTransportType::I2C) {
                reason = field.failure;
                return nullptr;
            }
            const uint32_t defaults[]{
                uint32_t(i),
                facts->channels[i], facts->internalBoost ? 0U : 1U, facts->internalBoost ? 0U : 1U, 2U,
                facts->inductor, facts->peak, facts->capacitor};
            field.values[i] = defaults[fieldIndex];
        }
    }
    uint32_t speakers[capacity]{};
    const bool hasSpeaker = properties.contains("cirrus,speaker-id");
    const char* speakerGPIOProperties[]{"spk-id-gpios", "speaker-id-gpios"};
    if (hasSpeaker && !properties.array("cirrus,speaker-id", speakers, context.count)) {
        reason = "CS35L41_SPEAKER_PROPERTIES_INVALID";
        return nullptr;
    }
    for (size_t i = 0; i < context.count; ++i) {
        bool speakerKnown = hasSpeaker;
        for (const auto* property : speakerGPIOProperties) {
            if (!properties.contains(property))
                continue;
            uint32_t observed = 0;
            if (!context.gpio || !context.gpio->read(property, i, observed)) {
                reason = "CS35L41_SPEAKER_ID_UNREADABLE";
                return nullptr;
            }
            if (speakerKnown && speakers[i] != observed) {
                reason = "CS35L41_SPEAKER_ID_MISMATCH";
                return nullptr;
            }
            speakers[i] = observed;
            speakerKnown = true;
        }
        if (!speakerKnown && facts && facts->speakerResource >= 0) {
            if (!context.gpio || !context.gpio->readResource(uint32_t(facts->speakerResource), 0, false, speakers[i])) {
                reason = context.gpio ? "CS35L41_SPEAKER_ID_UNREADABLE" : "CS35L41_SPEAKER_ID_REQUIRED";
                return nullptr;
            }
            speakerKnown = true;
        }
        if (!speakerKnown)
            speakers[i] = UINT32_MAX;
        if (speakerKnown && speakers[i] > UINT8_MAX) {
            reason = "CS35L41_SPEAKER_PROPERTIES_INVALID";
            return nullptr;
        }
        if (!available(context.identity.subsystemVendor, context.identity.subsystemDevice, speakers[i])) {
            reason = speakerKnown ? "CS35L41_SPEAKER_TUNING_UNAVAILABLE" : "CS35L41_GENERIC_TUNING_UNAVAILABLE";
            return nullptr;
        }
    }
    auto* result = new CS35L41BoardDiscovery;
    if (!result) {
        reason = "CS35L41_CONFIGURATION_ALLOCATION_FAILED";
        return nullptr;
    }
    if (context.endpoints[0].transport == core::BusTransportType::I2C) {
        result->resetDeclared = properties.contains("reset-gpios");
        result->resetResource = facts ? facts->resetResource : -1;
    }
    memcpy(result->hid, context.identity.acpiHid, strlen(context.identity.acpiHid) + 1);
    result->configuration = {{context.identity.subsystemVendor, context.identity.subsystemDevice, result->hid},
                             result->bindings,
                             context.count,
                             true,
                             true,
                             true,
                             true};
    bool consumed[capacity]{};
    bool usedChannel[2]{};
    for (size_t i = 0; i < context.count; ++i) {
        const auto& endpoint = context.endpoints[i];
        const uint32_t selection = endpoint.transport == core::BusTransportType::SPI ? endpoint.spiChipSelect : endpoint.i2cAddress;
        size_t index = 0;
        while (index < context.count && indices[index] != selection && (endpoint.transport != core::BusTransportType::I2C || indices[index] != i))
            ++index;
        if (!endpoint.valid() || (endpoint.transport == core::BusTransportType::SPI && endpoint.spiClockHz > 4000000) ||
            strlen(endpoint.controllerPath) >= sizeof(result->controllerPaths[i]) || index == context.count || consumed[index] ||
            channels[index] > 1 || usedChannel[channels[index]] || gpio1[index] > UINT8_MAX || gpio2[index] > UINT8_MAX) {
            reason = "CS35L41_ENDPOINT_PROPERTIES_MISMATCH";
            delete result;
            return nullptr;
        }
        consumed[index] = true;
        usedChannel[channels[index]] = true;
        auto& device = result->devices[i];
        if (const auto* correction = boardProperties(context.identity.subsystemVendor, context.identity.subsystemDevice)) {
            boost[index] = correction->internalBoost ? 0 : 1;
            inductor[index] = correction->inductor;
            peak[index] = correction->peak;
            capacitor[index] = correction->capacitor;
        }
        if (boost[index] > 1) {
            reason = "CS35L41_BOOST_TOPOLOGY_UNIMPLEMENTED";
            delete result;
            return nullptr;
        }
        device.power = {boost[index] == 0 ? BoostTopology::Internal : BoostTopology::ExternalWithSwitch,
                        inductor[index], peak[index], capacitor[index]};
        device.gpio1Function = uint8_t(gpio1[index]);
        device.gpio2Function = uint8_t(gpio2[index]);
        if (!device.valid()) {
            reason = "CS35L41_ELECTRICAL_PROPERTIES_UNSUPPORTED";
            delete result;
            return nullptr;
        }
        memcpy(result->controllerPaths[i], endpoint.controllerPath, strlen(endpoint.controllerPath) + 1);
        auto resolved = endpoint;
        resolved.controllerPath = result->controllerPaths[i];
        result->bindings[i] = {resolved,
                               registers::kValDeviceId,
                               channels[index],
                               speakers[index],
                               "speaker-protection",
                               result->hid,
                               uint8_t(gpio2[index] == 2 ? 2 : 0),
                               &device,
                               sizeof(device),
                               kDeviceConfigurationKind};
    }
    if (!result->configuration.permits(context.identity)) {
        reason = "CS35L41_CONFIGURATION_REJECTED";
        delete result;
        return nullptr;
    }
    reason = "CS35L41_ACPI_PROPERTIES_RESOLVED";
    return result;
}
}
