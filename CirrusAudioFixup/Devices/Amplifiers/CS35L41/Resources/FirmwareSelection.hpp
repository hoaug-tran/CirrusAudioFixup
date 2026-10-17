// Deterministic board/speaker selection. Names describe provenance; they never
// authorize a board or cause one OEM's coefficients to be reused for another.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace cirrus::devices::amplifiers::cs35l41 {
template <typename Resource>
const Resource* selectFirmwareResource(const Resource* resources, size_t count, uint16_t vendor, uint16_t device, uint32_t speaker) {
    if (!resources || !vendor || !device)
        return nullptr;
    const Resource* exact = nullptr;
    const Resource* generic = nullptr;
    unsigned exactCount = 0, genericCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& resource = resources[i];
        if (resource.subsystemVendor != vendor || resource.subsystemDevice != device)
            continue;
        if (resource.spkid == speaker) {
            exact = &resource;
            ++exactCount;
        } else if (!resource.spkid) {
            generic = &resource;
            ++genericCount;
        }
    }
    return exactCount ? (exactCount == 1 ? exact : nullptr) : (genericCount == 1 ? generic : nullptr);
}
}
