// Deterministic board/speaker selection. Names describe provenance; they never
// authorize a board or cause one OEM's coefficients to be reused for another.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace cirrus::devices::amplifiers::cs35l41 {
template <typename Resource>
const uint8_t* selectChannelCoefficients(const Resource& resource, bool right, size_t& bytes) {
    bytes = 0;
    if (resource.isDummy)
        return nullptr;
    const auto* data = right ? resource.binRight : resource.bin;
    const size_t size = right ? resource.binRightSize : resource.binSize;
    if (!data || !size || size % 4)
        return nullptr;
    bytes = size;
    return data;
}
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
        if (resource.genericSpeaker) {
            generic = &resource;
            ++genericCount;
        } else if (speaker != UINT32_MAX && resource.spkid == speaker) {
            exact = &resource;
            ++exactCount;
        }
    }
    return exactCount ? (exactCount == 1 ? exact : nullptr) : (genericCount == 1 ? generic : nullptr);
}
}
