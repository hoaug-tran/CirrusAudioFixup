// Self-contained CS35L41 factory. Register/host/board/firmware views are borrowed;
// the created driver owns every chip state and implementation method.
#pragma once
#include "Platform/AmplifierBackendRegistry.hpp"
namespace cirrus::devices::cs35l41 {
core::AmplifierLifecycleHooks* createBackend(const platform::AmplifierBackendContext&);
void destroyBackend(core::AmplifierLifecycleHooks*);
void inspectBackend(const platform::AmplifierBackendContext&);
bool validDebugPhase(const char*);
}
