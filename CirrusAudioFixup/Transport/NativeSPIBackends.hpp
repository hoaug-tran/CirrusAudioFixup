// Shipping communication-owner composition. Platform data may select its own
// factory; selection failure never falls back to a different owner/provider ABI.
#pragma once
#include "Transport/AMD/AMDNativeSPIBackend.hpp"
#include "Transport/Intel/IntelNativeSPIBackend.hpp"
namespace cirrus::transport {
inline NativeSPIControllerOwner* createNativeSPIBackend(IOACPIPlatformDevice* target, IOPCIDevice* pci, IOService* owner,
                                                        NativeSPIControllerFailure& failure) {
    return pci ? intel::createNativeSPIBackend(target, pci, owner, failure) : amd::createNativeSPIBackend(target, pci, owner, failure);
}
}
