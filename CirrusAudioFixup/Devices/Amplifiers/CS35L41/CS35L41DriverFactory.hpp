// Self-contained CS35L41 factory. Register/host/board/firmware views are borrowed;
// the created driver owns every chip state and implementation method.
#pragma once
#include "Runtime/AmplifierDriverRegistry.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
core::AmplifierLifecycleHooks* createDriver(const platform::AmplifierDriverContext&);
void destroyDriver(core::AmplifierLifecycleHooks*);
void inspectDevice(const platform::AmplifierDriverContext&);
bool validDebugPhase(const char*);
platform::DiscoveredBoard* discoverBoard(const platform::BoardDiscoveryContext&, const char*& reason);
bool protectionResourceAvailable(uint16_t vendor, uint16_t device, uint32_t speaker);
}
