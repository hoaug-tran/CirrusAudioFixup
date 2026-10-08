#pragma once

#include "Core/Types.hpp"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace cirrus::platform {

struct AmplifierEndpoint {
    const char* name;
    uint8_t address;
    core::AudioChannel channel;
};

struct ResetControllerQuirk {
    const char* controllerName;
    uint32_t registerIndex;
    uint32_t assertClearBit;
    uint32_t outputEnableBit;
    uint32_t minMemoryLength;
    uint32_t settleMs;
};

struct PlatformProfile {
    const char* name;
    const char* acpiHid;
    core::CodecModel amplifierModel;
    const AmplifierEndpoint* endpoints;
    size_t endpointCount;
    const ResetControllerQuirk* resetQuirk;
    bool allowAutomaticInitialization;
};

static constexpr AmplifierEndpoint kCs35l41FourChannelEndpoints[] = {
    {"left", 0x40, core::AudioChannel::Left},
    {"right", 0x41, core::AudioChannel::Right},
    {"top_left", 0x42, core::AudioChannel::LeftTweeter},
    {"top_right", 0x43, core::AudioChannel::RightTweeter},
};

static constexpr ResetControllerQuirk kAmdAmdi0030Reset = {
    "AMDI0030", 6, 22, 23, 0x400, 5,
};

static constexpr PlatformProfile kPlatformProfiles[] = {
    {"lenovo-clsa0100-cs35l41", "CLSA0100", core::CodecModel::CS35L41, kCs35l41FourChannelEndpoints,
     sizeof(kCs35l41FourChannelEndpoints) / sizeof(kCs35l41FourChannelEndpoints[0]), &kAmdAmdi0030Reset, true},
    {"lenovo-clsa0101-cs35l41", "CLSA0101", core::CodecModel::CS35L41, kCs35l41FourChannelEndpoints,
     sizeof(kCs35l41FourChannelEndpoints) / sizeof(kCs35l41FourChannelEndpoints[0]), nullptr, false},
    {"generic-csc3551-cs35l41", "CSC3551", core::CodecModel::CS35L41, kCs35l41FourChannelEndpoints,
     sizeof(kCs35l41FourChannelEndpoints) / sizeof(kCs35l41FourChannelEndpoints[0]), nullptr, false},
};

static constexpr PlatformProfile kSafeUnknownPlatform = {"unrecognized-fail-closed",
                                                         nullptr,
                                                         core::CodecModel::CS35L41,
                                                         kCs35l41FourChannelEndpoints,
                                                         sizeof(kCs35l41FourChannelEndpoints) / sizeof(kCs35l41FourChannelEndpoints[0]),
                                                         nullptr,
                                                         false};

inline const PlatformProfile* findPlatformProfile(const char* acpiHid) {
    if (!acpiHid)
        return nullptr;
    for (size_t i = 0; i < sizeof(kPlatformProfiles) / sizeof(kPlatformProfiles[0]); ++i) {
        if (strcmp(kPlatformProfiles[i].acpiHid, acpiHid) == 0)
            return &kPlatformProfiles[i];
    }
    return nullptr;
}

inline const PlatformProfile& defaultPlatformProfile() {
    return kSafeUnknownPlatform;
}

} // namespace cirrus::platform
