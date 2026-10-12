// Intel controller ownership module. Binding still validates the existing PCI IP.
#pragma once
#include "Transport/Intel/IntelLPSSSPIController.hpp"
#include "Transport/NativeSPIControllerOwner.hpp"
namespace cirrus::transport::intel {
class IntelNativeSPIBackend final : public NativeSPIControllerOwner {
    IntelLPSSSPIController mController;

public:
    IOReturn bind(IOPCIDevice* pci, IOService* owner) { return mController.bind(pci, owner); }
    SPIController& controller() override { return mController; }
    void suspend() override { mController.suspend(); }
    IOReturn resume(NativeSPIControllerFailure& failure) override {
        const auto result = mController.resume();
        failure = {"CONTROLLER_RESUME_FAILED", result, 0};
        return result;
    }
};
inline NativeSPIControllerOwner* createNativeSPIBackend(IOACPIPlatformDevice*, IOPCIDevice* pci, IOService* owner,
                                                        NativeSPIControllerFailure& failure) {
    failure.detail = pci ? (uint32_t(pci->configRead16(0)) << 16) | pci->configRead16(2) : 0;
    auto* backend = new IntelNativeSPIBackend;
    const auto result = backend ? backend->bind(pci, owner) : kIOReturnNoMemory;
    failure.result = result;
    failure.reason = result == kIOReturnUnsupported ? "INTEL_CONTROLLER_ID_UNSUPPORTED" : "INTEL_CONTROLLER_BIND_FAILED";
    if (result != kIOReturnSuccess) {
        delete backend;
        return nullptr;
    }
    return backend;
}
}
