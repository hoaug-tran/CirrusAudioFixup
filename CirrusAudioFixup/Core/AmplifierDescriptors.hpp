//
// AmplifierDescriptors.hpp
// Silicon identities accepted by this driver.
// An enum value is not hardware support. Add an entry only after its device
// driver and power, DSP and speaker-tuning paths have been implemented.
// See LICENSE for distribution terms.
//

#pragma once

#include "Core/AudioTypes.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

struct AmplifierDescriptor {
    AmplifierModel model;
    uint32_t deviceId;
    const char* name;
    bool hasHaloDsp;
};

static constexpr AmplifierDescriptor kSupportedAmplifiers[] = {
    {AmplifierModel::CS35L41, 0x00035A40, "CS35L41", true},
};

inline const AmplifierDescriptor* findAmplifierById(uint32_t deviceId) {
    for (size_t i = 0; i < sizeof(kSupportedAmplifiers) / sizeof(kSupportedAmplifiers[0]); ++i) {
        if (kSupportedAmplifiers[i].deviceId == deviceId)
            return &kSupportedAmplifiers[i];
    }
    return nullptr;
}

inline bool supportsAmplifierModel(AmplifierModel model) {
    for (size_t i = 0; i < sizeof(kSupportedAmplifiers) / sizeof(kSupportedAmplifiers[0]); ++i) {
        if (kSupportedAmplifiers[i].model == model)
            return true;
    }
    return false;
}

// End of the shared device registry.
}
