// Composition of complete, verified AMP modules. Add a module registration;
// do not put its silicon IDs, register policy or framing in the shared service.
#pragma once

#include "Devices/Amplifiers/CS35L41/CS35L41DriverRegistration.hpp"

namespace cirrus::devices {
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) { return amplifiers::cs35l41::registerDrivers(registry); }
}
