// CS35L56 module entry points; borrowed host/register views outlive inspection.
#pragma once
#include "Runtime/AmplifierDriverRegistry.hpp"
namespace cirrus::devices::amplifiers::cs35l56 {
void inspectDevice(const platform::AmplifierDriverContext&);
bool validDebugPhase(const char*);
platform::DiscoveredBoard* discoverBoard(const platform::BoardDiscoveryContext&, const char*& reason);
}
