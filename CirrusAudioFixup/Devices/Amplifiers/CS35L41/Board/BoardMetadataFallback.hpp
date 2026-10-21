// Fill missing board properties from verified firmware data.
// Resource indices select entries in the amplifier's evaluated _CRS.
#pragma once
#include "Platform/Board/BoardConfiguration.hpp"
#include "Platform/ACPI/ACPIBoardMetadata.hpp"
#include "Platform/ACPI/ACPISPIResources.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
inline bool prepareBoardMetadata(const platform::BoardIdentity& identity, IOACPIPlatformDevice* amplifier, IOService* owner) {
    struct Facts {
        uint32_t subsystem;
        uint16_t csResource, resetResource, speakerResource;
        uint32_t inductor, peak, capacitor;
    };
    // ASUS G614JI: Linux v6.12 electrical values and the board's stock _CRS order.
    static constexpr Facts boards[] = {{0x10431c9f, 0, 1, 2, 1000, 4500, 24}};
    const Facts* facts = nullptr;
    if (!identity.acpiHid || strcmp(identity.acpiHid, "CSC3551"))
        return true;
    for (const auto& board : boards)
        if (board.subsystem == (uint32_t(identity.subsystemVendor) << 16 | identity.subsystemDevice))
            facts = &board;
    if (!facts)
        return true;
    if (!amplifier || !owner)
        return false;
    OSObject* object = nullptr;
    const auto result = amplifier->evaluateObject("_CRS", &object);
    auto* bytes = OSDynamicCast(OSData, object);
    transport::ACPISPIResources resources{};
    const bool valid = result == kIOReturnSuccess && bytes &&
                       transport::parseACPISPIResources(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                                         resources) == transport::ACPISPIResult::Success &&
                       resources.count == 2 && resources.endpoints[0].endpoint.chipSelect == 0 &&
                       resources.endpoints[1].endpoint.chipSelect == 1 &&
                       !strcmp(resources.endpoints[0].source, resources.endpoints[1].source);
    OSSafeReleaseNULL(object);
    if (!valid)
        return false;
    auto* controller = transport::resolveACPIDevice(amplifier, resources.endpoints[0].source);
    if (!controller)
        return false;
    support::ACPIBoardMetadata metadata;
    auto* spi = metadata.node(controller);
    auto* amp = metadata.node(amplifier);
    const uint32_t indices[]{0, 1}, zero[]{0, 0}, two[]{2, 2};
    const uint32_t inductor[]{facts->inductor, facts->inductor}, peak[]{facts->peak, facts->peak};
    const uint32_t capacitor[]{facts->capacitor, facts->capacitor};
    const bool ok = spi && amp &&
        metadata.references(spi, "cs-gpios", amplifier, facts->csResource, 0, false, 1, 1) &&
        metadata.references(amp, "reset-gpios", amplifier, facts->resetResource, 0, false, 2) &&
        metadata.references(amp, "speaker-id-gpios", amplifier, facts->speakerResource, 0, false, 2) &&
        metadata.array(amp, "cirrus,dev-index", indices, 2) && metadata.array(amp, "cirrus,speaker-position", indices, 2) &&
        metadata.array(amp, "cirrus,boost-type", zero, 2) && metadata.array(amp, "cirrus,gpio1-func", zero, 2) &&
        metadata.array(amp, "cirrus,gpio2-func", two, 2) && metadata.array(amp, "cirrus,boost-ind-nanohenry", inductor, 2) &&
        metadata.array(amp, "cirrus,boost-peak-milliamp", peak, 2) && metadata.array(amp, "cirrus,boost-cap-microfarad", capacitor, 2) &&
        metadata.publish(owner);
    controller->release();
    if (ok)
        owner->setProperty("Cirrus_Board_Metadata_Source", "CS35L41_VERIFIED_BOARD_TABLE");
    return ok;
}
}
