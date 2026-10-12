// CS35L41-owned registration, silicon identity, bus framing and access policy.
// The composition registry only imports complete driver modules.
#pragma once

#include "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
constexpr bool cs35l41RegisterAccess(uint32_t reg) { return !(reg & 3) && reg <= 0x7ffffffc; }
constexpr core::RegisterMapConfig cs35l41RegisterFormat(bool spi) {
    auto format = spi ? CS35L41SPIRegisterIO::config() : core::RegisterMapConfig{};
    format.maxRegister = 0x7ffffffc;
    format.readable = cs35l41RegisterAccess;
    format.writable = cs35l41RegisterAccess;
    return format;
}
inline constexpr platform::AmplifierDriverRegistration kDriverRegistrations[] = {
    {"cs35l41-clsa0100", "CLSA0100", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase},
    {"cs35l41-clsa0101", "CLSA0101", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase},
    {"cs35l41-csc3551", "CSC3551", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase},
};
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) {
    for (const auto& definition : kDriverRegistrations)
        if (!registry.add(definition))
            return false;
    return true;
}
}
