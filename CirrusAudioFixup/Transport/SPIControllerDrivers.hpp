// Shipping communication-owner composition. Platform data may select its own
// factory; selection failure never falls back to a different owner/provider ABI.
#pragma once
#include "Transport/AMD/AMDSPIControllerOwner.hpp"
#include "Transport/Intel/IntelSPIControllerOwner.hpp"
namespace cirrus::transport {
inline bool nativeSPIControllerHasACPIContract(IOACPIPlatformDevice* target) { return amd::AMDSPIController::supportsProvider(target); }

inline NativeSPIControllerOwner* createNativeSPIController(IOACPIPlatformDevice* target, IOPCIDevice* pci, IOService* owner,
                                                           NativeSPIControllerFailure& failure) {
    if (pci)
        return intel::createNativeSPIController(target, pci, owner, failure);
    // Absence of a PCI association does not identify an AMD controller.
    // Only the AMD driver's existing ACPI identity contract authorizes it.
    if (nativeSPIControllerHasACPIContract(target))
        return amd::createNativeSPIController(target, pci, owner, failure);
    failure = {"PCI_ASSOCIATION_UNAVAILABLE_OR_ACPI_CONTROLLER_UNSUPPORTED", kIOReturnNotFound, 0};
    return nullptr;
}
}
