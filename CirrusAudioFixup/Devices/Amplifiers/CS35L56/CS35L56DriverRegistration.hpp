// CS35L56-owned registration, silicon identity, bus framing and access policy.
#pragma once
#include "Devices/Amplifiers/CS35L56/CS35L56DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
constexpr core::RegisterMapConfig cs35l56RegisterFormat() { return CS35L56SPIRegisterIO::config(); }
inline constexpr platform::AmplifierDriverRegistration kDriverRegistrations[] = {
    {"cs35l56-csc3556", "CSC3556", registers::kValDeviceId, 2, {}, cs35l56RegisterFormat(), nullptr, nullptr,
     false, registers::kRegDeviceId, registers::kDeviceIdMask, core::AmplifierModel::CS35L56, nullptr,
     inspectDevice, validDebugPhase, discoverBoard, nullptr, nullptr, true},
};
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) {
    for (const auto& definition : kDriverRegistrations)
        if (!registry.add(definition))
            return false;
    return true;
}
}
