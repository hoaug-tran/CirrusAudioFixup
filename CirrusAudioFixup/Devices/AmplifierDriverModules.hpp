// Register the amplifier modules included in this build.
// Each module supplies its own silicon IDs, register policy and framing.
#pragma once

#include "Devices/Amplifiers/CS35L41/CS35L41DriverRegistration.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56DriverRegistration.hpp"

namespace cirrus::devices {
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) {
    return amplifiers::cs35l41::registerDrivers(registry) && amplifiers::cs35l56::registerDrivers(registry);
}
}
