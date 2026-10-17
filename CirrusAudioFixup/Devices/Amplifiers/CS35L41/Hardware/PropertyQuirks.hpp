// Corrections for documented firmware defects only. Transport paths, channels,
// GPIO numbers and asset filenames must still come from their own providers.
// Reference: Linux v6.12 sound/pci/hda/cs35l41_hda_property.c.
#pragma once
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
struct PropertyCorrection {
    uint32_t subsystemId;
    PowerConfiguration power;
};
inline constexpr PropertyCorrection kPropertyCorrections[] = {
    {0x103c89c6, {BoostTopology::Internal, 1000, 4500, 24}},
};
inline const PropertyCorrection* propertyCorrection(uint16_t vendor, uint16_t device) {
    const uint32_t identity = uint32_t(vendor) << 16 | device;
    for (const auto& correction : kPropertyCorrections)
        if (correction.subsystemId == identity)
            return &correction;
    return nullptr;
}
}
