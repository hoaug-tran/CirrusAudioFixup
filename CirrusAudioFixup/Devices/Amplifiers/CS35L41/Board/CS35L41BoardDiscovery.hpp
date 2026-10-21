// Interpret the Cirrus HDA _DSD contract independently of OEM, machine name,
// controller namespace path and firmware filenames. Missing data fails closed.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Devices/Amplifiers/CS35L41/DeviceConfiguration.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/Board/BoardPropertyCorrections.hpp"
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
    for (const auto& field : required)
        if (!properties.array(field.name, field.values, context.count)) {
            reason = field.failure;
            return nullptr;
        }
    uint32_t speakers[capacity]{};
    const bool hasSpeaker = properties.contains("cirrus,speaker-id");
    const bool hasSpeakerGPIO = properties.contains("speaker-id-gpios");
    if (hasSpeaker && !properties.array("cirrus,speaker-id", speakers, context.count)) {
        reason = "CS35L41_SPEAKER_PROPERTIES_INVALID";
        return nullptr;
    }
    for (size_t i = 0; i < context.count; ++i) {
        if (hasSpeakerGPIO) {
            // Shared or per-endpoint input references are supplied by firmware
            // or verified module data; the actual level comes from the host.
            uint32_t observed = 0;
            if (!context.gpio || !context.gpio->read("speaker-id-gpios", i, observed)) {
                reason = "CS35L41_SPEAKER_ID_UNREADABLE";
                return nullptr;
            }
            if (hasSpeaker && speakers[i] != observed) {
                reason = "CS35L41_SPEAKER_ID_MISMATCH";
                return nullptr;
            }
            speakers[i] = observed;
        } else if (!hasSpeaker)
            speakers[i] = UINT32_MAX;
        if ((hasSpeaker || hasSpeakerGPIO) && speakers[i] > UINT8_MAX) {
            reason = "CS35L41_SPEAKER_PROPERTIES_INVALID";
            return nullptr;
        }
        if (!available(context.identity.subsystemVendor, context.identity.subsystemDevice, speakers[i])) {
            reason = hasSpeaker || hasSpeakerGPIO ? "CS35L41_SPEAKER_TUNING_UNAVAILABLE" : "CS35L41_GENERIC_TUNING_UNAVAILABLE";
            return nullptr;
        }
    }
    auto* result = new CS35L41BoardDiscovery;
    if (!result) {
        reason = "CS35L41_CONFIGURATION_ALLOCATION_FAILED";
        return nullptr;
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
        while (index < context.count && indices[index] != selection)
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
