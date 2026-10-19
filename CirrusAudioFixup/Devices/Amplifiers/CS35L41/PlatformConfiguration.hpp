// Interpret the Cirrus HDA _DSD contract independently of OEM, machine name,
// controller namespace path and firmware filenames. Missing data fails closed.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Devices/Amplifiers/CS35L41/DeviceConfiguration.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/PropertyQuirks.hpp"
#include "Platform/BoardDiscovery.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
using ResourceAvailable = bool (*)(uint16_t, uint16_t, uint32_t);
class PlatformConfiguration final : public platform::DiscoveredBoard {
public:
    static constexpr size_t kCapacity = 2; // Current resource schema has L0/R0.
    platform::AmplifierBinding bindings[kCapacity]{};
    DeviceConfiguration devices[kCapacity]{};
    char controllerPaths[kCapacity][256]{};
    char hid[32]{};
};
inline platform::DiscoveredBoard* resolvePlatformConfiguration(const platform::BoardDiscoveryContext& context,
                                                              ResourceAvailable available, const char*& reason) {
    reason = "CS35L41_PROPERTIES_INCOMPLETE";
    if (!context.identity.acpiHid || !context.identity.subsystemVendor || !context.identity.subsystemDevice ||
        strlen(context.identity.acpiHid) >= sizeof(PlatformConfiguration::hid) || !context.endpoints || !context.count ||
        context.count > PlatformConfiguration::kCapacity || !available)
        return nullptr;
    constexpr size_t capacity = PlatformConfiguration::kCapacity;
    uint32_t indices[capacity]{}, channels[capacity]{}, boost[capacity]{}, gpio1[capacity]{}, gpio2[capacity]{};
    uint32_t inductor[capacity]{}, peak[capacity]{}, capacitor[capacity]{};
    const auto& properties = context.properties;
    if (!properties.array("cirrus,dev-index", indices, context.count) ||
        !properties.array("cirrus,speaker-position", channels, context.count) ||
        !properties.array("cirrus,boost-type", boost, context.count) || !properties.array("cirrus,gpio1-func", gpio1, context.count) ||
        !properties.array("cirrus,gpio2-func", gpio2, context.count) ||
        !properties.array("cirrus,boost-ind-nanohenry", inductor, context.count) ||
        !properties.array("cirrus,boost-peak-milliamp", peak, context.count) ||
        !properties.array("cirrus,boost-cap-microfarad", capacitor, context.count))
        return nullptr;
    if (!available(context.identity.subsystemVendor, context.identity.subsystemDevice, UINT32_MAX)) {
        reason = "CS35L41_GENERIC_TUNING_UNAVAILABLE";
        return nullptr;
    }
    auto* result = new PlatformConfiguration;
    if (!result) {
        reason = "CS35L41_CONFIGURATION_ALLOCATION_FAILED";
        return nullptr;
    }
    memcpy(result->hid, context.identity.acpiHid, strlen(context.identity.acpiHid) + 1);
    result->configuration = {{context.identity.subsystemVendor, context.identity.subsystemDevice, result->hid},
                             result->bindings, context.count, true, true, true, true};
    bool consumed[capacity]{};
    bool usedChannel[2]{};
    for (size_t i = 0; i < context.count; ++i) {
        const auto& endpoint = context.endpoints[i];
        const uint32_t selection = endpoint.transport == core::BusTransportType::SPI ? endpoint.spiChipSelect : endpoint.i2cAddress;
        size_t index = 0;
        while (index < context.count && indices[index] != selection)
            ++index;
        if (!endpoint.valid() || (endpoint.transport == core::BusTransportType::SPI && endpoint.spiClockHz > 4000000) ||
            strlen(endpoint.controllerPath) >= sizeof(result->controllerPaths[i]) || index == context.count ||
            consumed[index] || channels[index] > 1 || usedChannel[channels[index]] || gpio1[index] > UINT8_MAX || gpio2[index] > UINT8_MAX) {
            reason = "CS35L41_ENDPOINT_PROPERTIES_MISMATCH";
            delete result;
            return nullptr;
        }
        consumed[index] = true;
        usedChannel[channels[index]] = true;
        auto& device = result->devices[i];
        // Subsystem-specific firmware correction, not a machine transport or
        // speaker map. Linux v6.12 cs35l41_hda_property.c documents this defect.
        if (const auto* correction = propertyCorrection(context.identity.subsystemVendor, context.identity.subsystemDevice)) {
            boost[index] = 0;
            inductor[index] = correction->power.inductorNanohenry;
            peak[index] = correction->power.peakMilliamp;
            capacitor[index] = correction->power.capacitorMicrofarad;
        }
        // Only the implemented internal topology may be discovered here.
        // External-with-switch/shared-boost require their own power contracts;
        // the legacy no-VSPK-switch mode is never inferred from ACPI value 1.
        if (boost[index] != 0) {
            reason = "CS35L41_BOOST_TOPOLOGY_UNIMPLEMENTED";
            delete result;
            return nullptr;
        }
        device.power = {BoostTopology::Internal, inductor[index], peak[index], capacitor[index]};
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
                               UINT32_MAX,
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
