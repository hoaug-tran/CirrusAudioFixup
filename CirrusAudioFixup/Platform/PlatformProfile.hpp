//
// PlatformProfile.hpp
// Board-specific amplifier endpoints and reset wiring.
// GPIO register offsets belong to a board profile, not a CPU family. Do not
// reuse the Legion reset quirk on another machine without checking its wiring.
// See LICENSE for distribution terms.
//

#pragma once
#include "Transport/NativeSPIControllerFactory.hpp"

#include "Core/AudioTypes.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include "Platform/BoardConfiguration.hpp"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class IOService;
namespace cirrus::transport {
class I2CController;
}

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

struct InterruptWiring {
    const char* name;
    uint32_t providerIndex;
    bool acpiNative;
    uint32_t gpioPin, gpioIRQ;
};
static constexpr InterruptWiring kLegionProtectionIRQ = {"GPIO84_PROTECTION_WITH_POLLING_BACKUP", 0, false, 84, 3};
struct PlatformProfile {
    const char* name;
    const char* acpiHid;
    core::AmplifierModel amplifierModel;
    const AmplifierEndpoint* endpoints;
    size_t endpointCount;
    const ResetControllerQuirk* resetQuirk;
    bool allowAutomaticInitialization;
    const BoardConfiguration* boardConfiguration{nullptr};
    const firmware::FirmwareBundle* firmwareBundles{nullptr};
    size_t firmwareBundleCount{0};
    const InterruptWiring* interruptWiring{nullptr};
    // Optional provider-backed communication adapter. Returns an owned object
    // implementing the same combined-read/bounds contract as the default ABI.
    // The provider owns PM; native MMIO/DMA owners require a separate PM/resource
    // contract and evidence before this entry may enable a board.
    transport::I2CController* (*i2cControllerFactory)(IOService*){nullptr};
    transport::NativeSPIControllerFactory spiControllerFactory{nullptr};
};

// Only endpoints with verified routing and per-speaker tuning may initialize.
static constexpr AmplifierEndpoint kCs35l41StereoEndpoints[] = {
    {"left", 0x40, core::AudioChannel::Left},
    {"right", 0x41, core::AudioChannel::Right},
};

static constexpr ResetControllerQuirk kAmdAmdi0030Reset = {
    "AMDI0030", 6, 22, 23, 0x400, 5,
};

static constexpr PlatformProfile kPlatformProfiles[] = {
    {"lenovo-clsa0100-cs35l41", "CLSA0100", core::AmplifierModel::CS35L41, kCs35l41StereoEndpoints, 2, &kAmdAmdi0030Reset, true, nullptr,
     nullptr, 0, &kLegionProtectionIRQ},
    {"lenovo-clsa0101-cs35l41", "CLSA0101", core::AmplifierModel::CS35L41, kCs35l41StereoEndpoints, 2, nullptr, false},
    {"generic-csc3551-cs35l41", "CSC3551", core::AmplifierModel::CS35L41, kCs35l41StereoEndpoints, 2, nullptr, false},
};

static constexpr PlatformProfile kSafeUnknownPlatform = {
    "unrecognized-fail-closed", nullptr, core::AmplifierModel::CS35L41, kCs35l41StereoEndpoints, 2, nullptr, false};

inline const PlatformProfile* findPlatformProfileIn(const PlatformProfile* profiles, size_t count, const char* acpiHid,
                                                    uint32_t subsystemId) {
    if (!profiles || !acpiHid)
        return nullptr;
    const PlatformProfile* fallback = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const auto& profile = profiles[i];
        if (!profile.acpiHid || strcmp(profile.acpiHid, acpiHid))
            continue;
        if (profile.boardConfiguration) {
            BoardIdentity actual{uint16_t(subsystemId >> 16), uint16_t(subsystemId), acpiHid};
            if (profile.boardConfiguration->permits(actual))
                return &profile;
        } else if (!fallback)
            fallback = &profile;
    }
    return fallback;
}
inline const PlatformProfile* findPlatformProfile(const char* acpiHid, uint32_t subsystemId = 0) {
    return findPlatformProfileIn(kPlatformProfiles, sizeof(kPlatformProfiles) / sizeof(kPlatformProfiles[0]), acpiHid, subsystemId);
}

// PCI subsystem identity is available before the codec publishes its SSID.
// This allowlist authorizes reference reset wiring, not generic CS35L41 support.
inline bool permitsHardwareInitialization(const PlatformProfile& profile, uint32_t pciSubsystemId) {
    if (profile.boardConfiguration) {
        BoardIdentity actual{uint16_t(pciSubsystemId >> 16), uint16_t(pciSubsystemId), profile.acpiHid};
        return profile.allowAutomaticInitialization && profile.boardConfiguration->permits(actual);
    }
    return profile.allowAutomaticInitialization && profile.acpiHid && strcmp(profile.acpiHid, "CLSA0100") == 0 &&
           (pciSubsystemId == 0x17AA382B || pciSubsystemId == 0x17AA3847);
}

inline const PlatformProfile& defaultPlatformProfile() { return kSafeUnknownPlatform; }

// End of the board profile definitions.
}
