#pragma once
#include "Devices/Amplifiers/CS35L56/CS35L56DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
constexpr core::RegisterMapConfig cs35l56RegisterFormat() { return CS35L56SPIRegisterIO::config(); }
inline constexpr platform::FirmwareResetSequence kFirmwareReset{"reset-gpios", false, 2, 3};
inline constexpr platform::AmplifierDriverRegistration kDriverRegistrations[] = {
    {"cs35l56-csc3556",
     "CSC3556",
     registers::kValDeviceId,
     2,
     {},
     cs35l56RegisterFormat(),
     createDriver,
     destroyDriver,
     true,
     registers::kRegDeviceId,
     registers::kDeviceIdMask,
     core::AmplifierModel::CS35L56,
     nullptr,
     inspectDevice,
     validDebugPhase,
     discoverBoard,
     &kFirmwareReset,
     nullptr,
     false},
};
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) {
    for (const auto& definition : kDriverRegistrations)
        if (!registry.add(definition))
            return false;
    return true;
}
}
