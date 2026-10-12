// AMD controller ownership module. The host does not drive a power parent;
// require observable D0 before binding and on every resume, as before extraction.
#pragma once
#include "Transport/AMD/AMDSPIController.hpp"
#include "Transport/NativeSPIControllerOwner.hpp"
namespace cirrus::transport::amd {
class AMDSPIControllerOwner final : public NativeSPIControllerOwner {
    AMDSPIController mController;
    IOACPIPlatformDevice* mTarget{nullptr}; // Borrowed host retain.
public:
    IOReturn bind(IOACPIPlatformDevice* target, IOService* owner, NativeSPIControllerFailure& failure) {
        if (!target || !owner) {
            failure = {"AMD_CONTROLLER_BIND_FAILED", kIOReturnBadArgument, 0};
            return failure.result;
        }
        uint32_t power = 0;
        if (target->evaluateInteger("_PSC", &power) != kIOReturnSuccess || power != 0) {
            failure = {"AMD_CONTROLLER_D0_UNVERIFIED", kIOReturnNotReady, power};
            return failure.result;
        }
        mTarget = target;
        const auto result = mController.bind(target, owner);
        failure = {result == kIOReturnUnsupported ? "AMD_CONTROLLER_HID_UNSUPPORTED" : "AMD_CONTROLLER_BIND_FAILED", result, 0};
        return result;
    }
    SPIController& controller() override { return mController; }
    void suspend() override { mController.suspend(); }
    IOReturn resume(NativeSPIControllerFailure& failure) override {
        uint32_t power = 0;
        if (!mTarget || mTarget->evaluateInteger("_PSC", &power) != kIOReturnSuccess || power != 0) {
            failure = {"AMD_RESUME_D0_UNVERIFIED", kIOReturnNotReady, power};
            return failure.result;
        }
        const auto result = mController.resume();
        failure = {"CONTROLLER_RESUME_FAILED", result, 0};
        return result;
    }
};
inline NativeSPIControllerOwner* createNativeSPIController(IOACPIPlatformDevice* target, IOPCIDevice*, IOService* owner,
                                                           NativeSPIControllerFailure& failure) {
    auto* driver = new AMDSPIControllerOwner;
    if (!driver) {
        failure = {"AMD_CONTROLLER_BIND_FAILED", kIOReturnNoMemory, 0};
        return nullptr;
    }
    if (driver->bind(target, owner, failure) != kIOReturnSuccess) {
        delete driver;
        return nullptr;
    }
    return driver;
}
}
