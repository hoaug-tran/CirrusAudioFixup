// Fill missing board properties from verified firmware data.
// Resource indices select entries in the amplifier's evaluated _CRS.
#pragma once
#include "Devices/Amplifiers/CS35L41/Board/BoardProperties.hpp"
#include "Platform/ACPI/ACPIBoardMetadata.hpp"
#include "Platform/ACPI/ACPISPIResources.hpp"
#include "Platform/Board/BoardConfiguration.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
inline bool prepareBoardMetadata(const platform::BoardIdentity& identity, IOACPIPlatformDevice* amplifier, IOService* owner) {
    if (!identity.acpiHid || strcmp(identity.acpiHid, "CSC3551"))
        return true;
    const auto* facts = boardProperties(identity.subsystemVendor, identity.subsystemDevice);
    if (!facts)
        return true;
    if (!amplifier || !owner)
        return false;
    // Keep native firmware validation independent of table selection. Linux
    // installs driver GPIO mappings only when device properties are absent.
    transport::ACPIDeviceProperties native;
    const auto nativeResult = native.load(amplifier);
    if (nativeResult != kIOReturnSuccess && !native.missingNativeMetadata())
        return false;
    const bool mapGPIO = !native.hasNativeProperties();
    if (!facts->internalBoost || facts->count != 2) {
        owner->setProperty("Cirrus_Board_Metadata_Reason", "CS35L41_BOARD_TOPOLOGY_UNIMPLEMENTED");
        return false;
    }
    OSObject* object = nullptr;
    const auto result = amplifier->evaluateObject("_CRS", &object);
    auto* bytes = OSDynamicCast(OSData, object);
    auto* resources = new transport::ACPISPIResources{};
    const bool valid = resources && result == kIOReturnSuccess && bytes &&
                       transport::parseACPISPIResources(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                                        *resources) == transport::ACPISPIResult::Success &&
                       resources->count == 2 && resources->endpoints[0].endpoint.chipSelect <= 1 &&
                       resources->endpoints[1].endpoint.chipSelect <= 1 &&
                       resources->endpoints[0].endpoint.chipSelect != resources->endpoints[1].endpoint.chipSelect &&
                       !strcmp(resources->endpoints[0].source, resources->endpoints[1].source);
    auto* controller = valid ? transport::resolveACPIDevice(amplifier, resources->endpoints[0].source) : nullptr;
    delete resources;
    OSSafeReleaseNULL(object);
    if (!controller)
        return false;
    support::ACPIBoardMetadata metadata;
    auto* spi = metadata.node(controller);
    auto* amp = metadata.node(amplifier);
    const uint32_t indices[]{0, 1}, two[]{2, 2};
    const uint32_t boostType[]{facts->internalBoost ? 0U : 1U, facts->internalBoost ? 0U : 1U};
    const uint32_t gpio1Func[]{facts->internalBoost ? 0U : 1U, facts->internalBoost ? 0U : 1U};
    const uint32_t channels[]{facts->channels[0], facts->channels[1]};
    const uint32_t inductor[]{facts->inductor, facts->inductor}, peak[]{facts->peak, facts->peak};
    const uint32_t capacitor[]{facts->capacitor, facts->capacitor};
    const bool ok = spi && amp &&
                    (!mapGPIO || facts->csResource < 0 ||
                     metadata.references(spi, "cs-gpios", amplifier, uint16_t(facts->csResource), 0, false, 1, 1)) &&
                    (!mapGPIO || facts->resetResource < 0 ||
                     metadata.references(amp, "reset-gpios", amplifier, uint16_t(facts->resetResource), 0, false, 2)) &&
                    (!mapGPIO || facts->speakerResource < 0 ||
                     metadata.references(amp, "speaker-id-gpios", amplifier, uint16_t(facts->speakerResource), 0, false, 2)) &&
                    metadata.array(amp, "cirrus,dev-index", indices, 2) && metadata.array(amp, "cirrus,speaker-position", channels, 2) &&
                    metadata.array(amp, "cirrus,boost-type", boostType, 2) && metadata.array(amp, "cirrus,gpio1-func", gpio1Func, 2) &&
                    metadata.array(amp, "cirrus,gpio2-func", two, 2) && metadata.array(amp, "cirrus,boost-ind-nanohenry", inductor, 2) &&
                    metadata.array(amp, "cirrus,boost-peak-milliamp", peak, 2) &&
                    metadata.array(amp, "cirrus,boost-cap-microfarad", capacitor, 2) && metadata.publish(owner);
    controller->release();
    if (ok)
        owner->setProperty("Cirrus_Board_Metadata_Source", "CS35L41_VERIFIED_BOARD_TABLE");
    return ok;
}
}
