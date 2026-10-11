// Production lifecycle factory. The host supplies board/resource ownership;
// register transactions and lifecycle selection remain transport-neutral.
#pragma once
#include "Platform/BackendRegistry.hpp"
namespace cirrus::devices::cs35l41 {
core::AmplifierLifecycleHooks* createServiceBackend(const platform::BackendContext&);
void destroyServiceBackend(core::AmplifierLifecycleHooks*);
}
