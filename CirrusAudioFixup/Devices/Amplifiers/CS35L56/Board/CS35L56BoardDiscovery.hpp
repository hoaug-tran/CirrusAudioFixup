// Resolve firmware wiring and live speaker identity before authorizing writes.
#pragma once
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareSelection.hpp"
#include "Platform/Board/BoardDiscovery.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
struct BoardProperties {
    uint32_t subsystem;
    core::BusTransportType transport;
    uint8_t count;
    uint16_t addresses[2];
    uint8_t channels[2];
    uint32_t speakerId;
    int8_t resetResource;
};

inline constexpr BoardProperties kBoardProperties[] = {
    {0x17aa391e, core::BusTransportType::I2C, 2, {0x31, 0x30}, {0, 1}, 0, -1},
    {0x17aa390b, core::BusTransportType::I2C, 2, {0x31, 0x30}, {0, 1}, 0, -1},
};

inline const BoardProperties* boardProperties(uint16_t vendor, uint16_t device) {
    const uint32_t subsystem = (uint32_t(vendor) << 16) | device;
    for (const auto& facts : kBoardProperties)
        if (facts.subsystem == subsystem)
            return &facts;
    return nullptr;
}

class CS35L56BoardDiscovery final : public platform::DiscoveredBoard {
public:
    platform::AmplifierBinding bindings[2]{};
    char controllerPaths[2][256]{};
    char hid[32]{};
    bool resetDeclared{false};
    int32_t resetResource{-1};
    ~CS35L56BoardDiscovery() override = default;
    bool prepareHardware(platform::BoardDiscoveryContext::GPIOAccess& gpio) const override {
        return (!resetDeclared && resetResource < 0) || gpio.reset(resetDeclared ? "reset-gpios" : nullptr, resetResource);
    }
};

inline platform::DiscoveredBoard* resolveBoardConfiguration(const platform::BoardDiscoveryContext& context, const char*& reason) {
    reason = "CS35L56_PROPERTIES_INCOMPLETE";
    if (!context.identity.acpiHid || strlen(context.identity.acpiHid) >= sizeof(CS35L56BoardDiscovery::hid) ||
        strcmp(context.identity.acpiHid, "CSC3556") || !context.endpoints || context.count < 2)
        return nullptr;

    const auto* facts = boardProperties(context.identity.subsystemVendor, context.identity.subsystemDevice);
    if (facts) {
        if (facts->transport != core::BusTransportType::I2C || facts->count != 2 || context.count < facts->count) {
            reason = "CS35L56_BOARD_TOPOLOGY_UNIMPLEMENTED";
            return nullptr;
        }
        const auto* resource = selectFirmwareResource(cs35l56Firmware, cs35l56FirmwareCount, context.identity.subsystemVendor,
                                                      context.identity.subsystemDevice, facts->speakerId);
        if (!resource || resource->isDummy) {
            reason = "CS35L56_SPEAKER_TUNING_UNAVAILABLE";
            return nullptr;
        }
        auto* result = new CS35L56BoardDiscovery;
        if (!result) {
            reason = "CS35L56_CONFIGURATION_ALLOCATION_FAILED";
            return nullptr;
        }
        memcpy(result->hid, context.identity.acpiHid, strlen(context.identity.acpiHid) + 1);
        result->resetResource = facts->resetResource;
        result->resetDeclared = context.properties.contains("reset-gpios");
        result->configuration = {
            {context.identity.subsystemVendor, context.identity.subsystemDevice, result->hid}, result->bindings, 2, true, true, true, true};

        const char* firstController = nullptr;
        for (size_t channel = 0; channel < 2; ++channel) {
            const uint16_t expectedAddress = facts->addresses[channel];
            const core::BusEndpoint* matchedEndpoint = nullptr;
            for (size_t i = 0; i < context.count; ++i) {
                if (context.endpoints[i].valid() && context.endpoints[i].transport == facts->transport &&
                    context.endpoints[i].i2cAddress == expectedAddress && !context.endpoints[i].tenBitAddress) {
                    matchedEndpoint = &context.endpoints[i];
                    break;
                }
            }
            if (!matchedEndpoint || strlen(matchedEndpoint->controllerPath) >= sizeof(result->controllerPaths[channel])) {
                reason = "CS35L56_ENDPOINT_PROPERTIES_MISMATCH";
                delete result;
                return nullptr;
            }
            if (!firstController)
                firstController = matchedEndpoint->controllerPath;
            else if (strcmp(firstController, matchedEndpoint->controllerPath)) {
                reason = "CS35L56_ENDPOINT_PROPERTIES_MISMATCH";
                delete result;
                return nullptr;
            }
            memcpy(result->controllerPaths[channel], matchedEndpoint->controllerPath, strlen(matchedEndpoint->controllerPath) + 1);
            auto resolved = *matchedEndpoint;
            resolved.controllerPath = result->controllerPaths[channel];
            result->bindings[channel] = {resolved, registers::kValDeviceId, facts->channels[channel], facts->speakerId, "speaker-protection", result->hid};
        }
        if (!result->configuration.permits(context.identity)) {
            delete result;
            reason = "CS35L56_CONFIGURATION_REJECTED";
            return nullptr;
        }
        reason = "CS35L56_ACPI_PROPERTIES_RESOLVED";
        return result;
    }

    if (context.count != 2 || !context.gpio || !context.properties.contains("spk-id-gpios"))
        return nullptr;
    uint32_t indices[2]{}, boost[2]{};
    if (!context.properties.array("cirrus,dev-index", indices, 2) || !context.properties.array("cirrus,boost-type", boost, 2))
        return nullptr;
    uint32_t speaker = 0, next = 0;
    for (size_t i = 0; i < 2; ++i) {
        if (!context.gpio->read("spk-id-gpios", i, next) || next > 1 || (i && next != speaker)) {
            reason = "CS35L56_SPEAKER_ID_UNAVAILABLE_OR_INCONSISTENT";
            return nullptr;
        }
        speaker = next;
    }
    const auto* resource = selectFirmwareResource(cs35l56Firmware, cs35l56FirmwareCount, context.identity.subsystemVendor,
                                                  context.identity.subsystemDevice, speaker);
    if (!resource || resource->isDummy) {
        reason = "CS35L56_SPEAKER_TUNING_UNAVAILABLE";
        return nullptr;
    }
    auto* result = new CS35L56BoardDiscovery;
    if (!result) {
        reason = "CS35L56_CONFIGURATION_ALLOCATION_FAILED";
        return nullptr;
    }
    memcpy(result->hid, context.identity.acpiHid, strlen(context.identity.acpiHid) + 1);
    result->resetDeclared = context.properties.contains("reset-gpios");
    result->configuration = {
        {context.identity.subsystemVendor, context.identity.subsystemDevice, result->hid}, result->bindings, 2, true, true, true, true};
    bool used[2]{};
    for (size_t i = 0; i < 2; ++i) {
        const auto& endpoint = context.endpoints[i];
        const auto cs = endpoint.spiChipSelect;
        size_t propertyIndex = indices[0] == cs ? 0 : indices[1] == cs ? 1 : 2;
        if (!endpoint.valid() || endpoint.transport != core::BusTransportType::SPI || cs > 1 || used[cs] || propertyIndex == 2 ||
            boost[propertyIndex] || indices[0] == indices[1] || strlen(endpoint.controllerPath) >= sizeof(result->controllerPaths[i]) ||
            strcmp(endpoint.controllerPath, context.endpoints[0].controllerPath)) {
            reason = "CS35L56_ENDPOINT_PROPERTIES_MISMATCH";
            delete result;
            return nullptr;
        }
        used[cs] = true;
        memcpy(result->controllerPaths[i], endpoint.controllerPath, strlen(endpoint.controllerPath) + 1);
        auto resolved = endpoint;
        resolved.controllerPath = result->controllerPaths[i];
        // Verified firmware uses physical amp index 0/1 as left/right channel.
        result->bindings[i] = {resolved, registers::kValDeviceId, cs, speaker, "speaker-protection", result->hid};
    }
    if (!result->configuration.permits(context.identity)) {
        delete result;
        reason = "CS35L56_CONFIGURATION_REJECTED";
        return nullptr;
    }
    reason = "CS35L56_ACPI_PROPERTIES_RESOLVED";
    return result;
}
}
