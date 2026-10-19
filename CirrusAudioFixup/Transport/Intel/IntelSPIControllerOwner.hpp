// Intel controller ownership module. Binding still validates the existing PCI IP.
#pragma once
#if defined(KERNEL)
#include "Support/Logging.hpp"
#endif
#include "Transport/Intel/IntelLPSSSPIController.hpp"
#include "Transport/NativeSPIControllerOwner.hpp"
namespace cirrus::transport::intel {
class IntelSPIControllerOwner final : public NativeSPIControllerOwner {
    IntelLPSSSPIController mController;

public:
    IOReturn bind(IOPCIDevice* pci, IOService* owner) { return mController.bind(pci, owner); }
#if defined(KERNEL)
    IOReturn configureChipSelects(IOACPIPlatformDevice* target, IOService* owner) {
        return mController.configureChipSelects(target, owner);
    }
    IOReturn prepareGPIOGroup(IOACPIPlatformDevice* source, const char* property) override {
        return mController.prepareGPIOGroup(source, property);
    }
    IOReturn writeGPIOGroup(bool level) override { return mController.writeGPIOGroup(level); }
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
    auto result = driver ? driver->bind(pci, owner) : kIOReturnNoMemory;
    failure.result = result;
    failure.reason = driver ? driver->bindFailureReason() : "INTEL_CONTROLLER_ALLOCATION_FAILED";
#if defined(KERNEL)
    if (result == kIOReturnSuccess) {
        result = driver->configureChipSelects(target, owner);
        failure.result = result;
        if (result != kIOReturnSuccess)
            failure.reason = "INTEL_SPI_CHIP_SELECT_CONFIGURATION_FAILED";
    }
    if (owner && gCirrusDebug) {
        owner->setProperty("Cirrus_Intel_SPI_Bind_Reason", failure.reason);
        owner->setProperty("Cirrus_Intel_SPI_PCI_Identity", uint64_t(failure.detail), 32);
        if (driver) {
            owner->setProperty("Cirrus_Intel_SPI_Initial_Capabilities", uint64_t(driver->initialCapabilities()), 32);
            owner->setProperty("Cirrus_Intel_SPI_Initial_Status", uint64_t(driver->initialStatus()), 32);
            owner->setProperty("Cirrus_Intel_SPI_Initial_Clock", uint64_t(driver->initialClock()), 32);
            owner->setProperty("Cirrus_Intel_SPI_Prepared_Clock", uint64_t(driver->preparedClock()), 32);
            owner->setProperty("Cirrus_Intel_SPI_Initial_Clock_Gate", uint64_t(driver->initialClockGate()), 32);
        }
    }
#else
    (void)target;
#endif
    if (result != kIOReturnSuccess) {
        delete driver;
        return nullptr;
    }
    return driver;
}
}
