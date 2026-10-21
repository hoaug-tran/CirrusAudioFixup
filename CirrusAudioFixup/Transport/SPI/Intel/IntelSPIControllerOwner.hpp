// Intel controller ownership module. Binding still validates the existing PCI IP.
#pragma once
#if defined(KERNEL)
#include "Support/Logging.hpp"
#endif
#include "Transport/SPI/Intel/IntelLPSSSPIController.hpp"
#include "Transport/SPI/NativeSPIControllerOwner.hpp"
namespace cirrus::transport::intel {
class IntelSPIControllerOwner final : public NativeSPIControllerOwner {
    IntelLPSSSPIController mController;
    // NativeSPIHost retains the target until this owner is destroyed.
    IOACPIPlatformDevice* mTarget{nullptr};
    IOService* mOwner{nullptr};
#if defined(KERNEL)
    bool mChipSelectsReady{false};
#endif
    uint32_t mPCIIdentity{0};

public:
    IOReturn bind(IOPCIDevice* pci, IOService* owner) {
        mPCIIdentity = pci ? (uint32_t(pci->configRead16(0)) << 16) | pci->configRead16(2) : 0;
        return mController.bind(pci, owner);
    }
#if defined(KERNEL)
    void publishEvidence(const char* reason) {
        if (mOwner && gCirrusDebug) {
            mOwner->setProperty("Cirrus_Intel_SPI_Bind_Reason", mController.powerPending() ? "WAITING_POWER" : reason);
            mOwner->setProperty("Cirrus_Intel_SPI_PCI_Identity", uint64_t(mPCIIdentity), 32);
            if (!mController.powerPending()) {
                mOwner->setProperty("Cirrus_Intel_SPI_Initial_Capabilities", uint64_t(mController.initialCapabilities()), 32);
                mOwner->setProperty("Cirrus_Intel_SPI_Initial_Status", uint64_t(mController.initialStatus()), 32);
                mOwner->setProperty("Cirrus_Intel_SPI_Initial_Clock", uint64_t(mController.initialClock()), 32);
                mOwner->setProperty("Cirrus_Intel_SPI_Prepared_Clock", uint64_t(mController.preparedClock()), 32);
                mOwner->setProperty("Cirrus_Intel_SPI_Initial_Clock_Gate", uint64_t(mController.initialClockGate()), 32);
            }
        }
    }
#endif
    void setContext(IOACPIPlatformDevice* target, IOService* owner) {
        mTarget = target;
        mOwner = owner;
    }
    bool powerPending() const override { return mController.powerPending(); }
    IOReturn continueBind(NativeSPIControllerFailure& failure) override {
        auto result = mController.continueBind();
        failure.result = result;
        failure.reason = mController.bindFailureReason();
#if defined(KERNEL)
        if (result == kIOReturnSuccess && !mChipSelectsReady) {
            result = configureChipSelects(mTarget, mOwner);
            mChipSelectsReady = result == kIOReturnSuccess;
            failure.result = result;
            if (result != kIOReturnSuccess)
                failure.reason = "INTEL_SPI_CHIP_SELECT_CONFIGURATION_FAILED";
            publishEvidence(failure.reason);
        }
#endif
        return result;
    }
#if defined(KERNEL)
    IOReturn configureChipSelects(IOACPIPlatformDevice* target, IOService* owner) {
        return mController.configureChipSelects(target, owner);
    }
    IOReturn prepareGPIOGroup(IOACPIPlatformDevice* source, const char* property) override {
        return mController.prepareGPIOGroup(source, property);
    }
    IOReturn writeGPIOGroup(bool level) override { return mController.writeGPIOGroup(level); }
    IOReturn readGPIO(IOACPIPlatformDevice* source, const char* property, size_t index, uint32_t& value) override {
        return mController.readGPIO(source, property, index, value);
    }
#endif
    const char* bindFailureReason() const { return mController.bindFailureReason(); }
    uint32_t initialCapabilities() const { return mController.initialCapabilities(); }
    uint32_t initialStatus() const { return mController.initialStatus(); }
    uint32_t initialClock() const { return mController.initialClock(); }
    uint32_t preparedClock() const { return mController.preparedClock(); }
    uint32_t initialClockGate() const { return mController.initialClockGate(); }
    SPIController& controller() override { return mController; }
    void suspend() override { mController.suspend(); }
    IOReturn resume(NativeSPIControllerFailure& failure) override {
        const auto result = mController.resume();
        failure = {"CONTROLLER_RESUME_FAILED", result, 0};
        return result;
    }
};
inline NativeSPIControllerOwner* createNativeSPIController(IOACPIPlatformDevice* target, IOPCIDevice* pci, IOService* owner,
                                                           NativeSPIControllerFailure& failure) {
    failure.detail = pci ? (uint32_t(pci->configRead16(0)) << 16) | pci->configRead16(2) : 0;
    auto* driver = new IntelSPIControllerOwner;
    if (driver)
        driver->setContext(target, owner);
    auto result = driver ? driver->bind(pci, owner) : kIOReturnNoMemory;
    failure.result = result;
    failure.reason = driver ? driver->bindFailureReason() : "INTEL_CONTROLLER_ALLOCATION_FAILED";
#if defined(KERNEL)
    if (result == kIOReturnSuccess)
        result = driver->continueBind(failure);
    if (driver)
        driver->publishEvidence(failure.reason);
#else
    (void)target;
#endif
    if (result != kIOReturnSuccess && !(driver && driver->powerPending())) {
        delete driver;
        return nullptr;
    }
    return driver;
}
}
