// Composition of complete, verified AMP modules. Add a module registration;
// do not put its silicon IDs, register policy or framing in the shared service.
#pragma once

#include "Devices/CS35L41/Definition.hpp"

namespace cirrus::devices {
inline bool registerBackends(platform::AmplifierBackendRegistry& registry) { return cs35l41::registerBackends(registry); }
}
