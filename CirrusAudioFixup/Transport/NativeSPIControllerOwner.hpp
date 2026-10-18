// Owned native controller resources and PM, independent of AMP/register framing.
// The host retains the ACPI target and lends discovery/owner views until release.
#pragma once
#include "Transport/NativeSPIControllerFactory.hpp"
#include "Transport/SPIController.hpp"
namespace cirrus::transport {
// Failure reason strings have static lifetime, including after failed owner release.
struct NativeSPIControllerFailure {
    const char* reason{"CONTROLLER_FACTORY_FAILED"};
    IOReturn result{kIOReturnNotReady};
    uint32_t detail{0};
};
class NativeSPIControllerOwner {
public:
    NativeSPIControllerOwner() = default;
    NativeSPIControllerOwner(const NativeSPIControllerOwner&) = delete;
    NativeSPIControllerOwner& operator=(const NativeSPIControllerOwner&) = delete;
    virtual ~NativeSPIControllerOwner() = default;
    virtual SPIController& controller() = 0;
    virtual IOReturn prepareGPIOGroup(IOACPIPlatformDevice*, const char*) { return kIOReturnUnsupported; }
    virtual IOReturn writeGPIOGroup(bool) { return kIOReturnUnsupported; }
    virtual void suspend() = 0;
    virtual IOReturn resume(NativeSPIControllerFailure&) = 0;
};
}
