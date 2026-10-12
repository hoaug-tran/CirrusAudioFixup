// Self-contained CS35L41 factory. Register/host/board/firmware views are borrowed;
// the created driver owns every chip state and implementation method.
#pragma once
#include "Platform/AmplifierDriverRegistry.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
core::AmplifierLifecycleHooks* createDriver(const platform::AmplifierDriverContext&);
void destroyDriver(core::AmplifierLifecycleHooks*);
void inspectDevice(const platform::AmplifierDriverContext&);
bool validDebugPhase(const char*);
}
