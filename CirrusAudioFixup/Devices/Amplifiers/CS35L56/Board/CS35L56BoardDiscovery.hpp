// Resolve firmware wiring and live speaker identity before authorizing writes.
#pragma once
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareSelection.hpp"
#include "Platform/Board/BoardDiscovery.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56BoardDiscovery final : public platform::DiscoveredBoard {
public:
    platform::AmplifierBinding bindings[2]{};
    char controllerPaths[2][256]{};
    char hid[32]{};
};
inline platform::DiscoveredBoard* resolveBoardConfiguration(const platform::BoardDiscoveryContext& context, const char*& reason) {
    reason = "CS35L56_PROPERTIES_INCOMPLETE";
    if (!context.identity.acpiHid || strcmp(context.identity.acpiHid, "CSC3556") || !context.endpoints || context.count != 2 ||
        !context.gpio || !context.properties.contains("spk-id-gpios"))
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
