// Shipping communication-owner composition. Platform data may select its own
// factory; selection failure never falls back to a different owner/provider ABI.
#pragma once
#include "Transport/AMD/AMDSPIControllerOwner.hpp"
#include "Transport/Intel/IntelSPIControllerOwner.hpp"
namespace cirrus::transport {
inline NativeSPIControllerOwner* createNativeSPIController(IOACPIPlatformDevice* target, IOPCIDevice* pci, IOService* owner,
                                                           NativeSPIControllerFailure& failure) {
    return pci ? intel::createNativeSPIController(target, pci, owner, failure)
               : amd::createNativeSPIController(target, pci, owner, failure);
}
}
