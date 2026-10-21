// CS35L41-owned registration, silicon identity, bus framing and access policy.
// The composition registry only imports complete driver modules.
#pragma once

#include "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#if defined(KERNEL)
#include "Devices/Amplifiers/CS35L41/Board/BoardMetadataFallback.hpp"
#endif

namespace cirrus::devices::amplifiers::cs35l41 {
constexpr bool cs35l41RegisterAccess(uint32_t reg) { return !(reg & 3) && reg <= registers::kLastRegister; }
constexpr core::RegisterMapConfig cs35l41RegisterFormat(bool spi) {
    auto format = spi ? CS35L41SPIRegisterIO::config() : core::RegisterMapConfig{};
    format.maxRegister = registers::kLastRegister;
    format.readable = cs35l41RegisterAccess;
    format.writable = cs35l41RegisterAccess;
    return format;
}
// Linux cs35l41_hda_probe: shared logical reset low for 2 ms, then high,
// followed by at least 2 ms before any register access.
inline constexpr platform::FirmwareResetSequence kFirmwareReset{"reset-gpios", false, 2, 2};
inline constexpr platform::AmplifierDriverRegistration kDriverRegistrations[] = {
    {"cs35l41-clsa0100", "CLSA0100", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase},
    {"cs35l41-clsa0101", "CLSA0101", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase},
    {"cs35l41-csc3551", "CSC3551", registers::kValDeviceId, 3, cs35l41RegisterFormat(false), cs35l41RegisterFormat(true), createDriver,
     destroyDriver, true, 0, 0xffffffffU, core::AmplifierModel::CS35L41, nullptr, inspectDevice, validDebugPhase, discoverBoard,
     &kFirmwareReset,
#if defined(KERNEL)
     prepareBoardMetadata
#else
     nullptr
#endif
    },
};
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) {
    for (const auto& definition : kDriverRegistrations)
        if (!registry.add(definition))
            return false;
    return true;
}
}
