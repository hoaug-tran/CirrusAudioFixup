//
// DeviceRegistry.hpp
// Silicon identities accepted by this driver.
// An enum value is not hardware support. Add an entry only after its device
// backend and power, DSP and speaker-tuning paths have been implemented.
// See LICENSE for distribution terms.
//

#pragma once

#include "Core/Types.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

struct DeviceDescriptor {
    CodecModel model;
    uint32_t deviceId;
    const char* name;
    bool hasHaloDsp;
};

static constexpr DeviceDescriptor kSupportedDevices[] = {
    {CodecModel::CS35L41, 0x00035A40, "CS35L41", true},
};

inline const DeviceDescriptor* findDeviceById(uint32_t deviceId) {
    for (size_t i = 0; i < sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]); ++i) {
        if (kSupportedDevices[i].deviceId == deviceId)
            return &kSupportedDevices[i];
    }
    return nullptr;
}

inline bool supportsDevice(CodecModel model) {
    for (size_t i = 0; i < sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]); ++i) {
        if (kSupportedDevices[i].model == model)
            return true;
    }
    return false;
}

// End of the shared device registry.
}
