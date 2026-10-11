// Production lifecycle factory. The host supplies board/resource ownership;
// register transactions and lifecycle selection remain transport-neutral.
#pragma once
#include "Platform/AmplifierBackendRegistry.hpp"
namespace cirrus::devices::cs35l41 {
core::AmplifierLifecycleHooks* createBackend(const platform::AmplifierBackendContext&);
void destroyBackend(core::AmplifierLifecycleHooks*);
}
