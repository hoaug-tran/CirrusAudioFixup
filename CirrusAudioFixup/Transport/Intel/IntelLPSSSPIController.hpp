// PCI/MMIO owner for profiled Intel LPSS SPI controller families.
// The enclosing IOService supplies PCI D0 and quiesces clients before teardown.
// No automatic PCI/ACPI matching or amplifier board profile is implied.
#pragma once

#include "Transport/Intel/LPSSSPIEngine.hpp"
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#if defined(KERNEL)
#include "Transport/Intel/IntelSPIChipSelects.hpp"
#endif

namespace cirrus::transport::intel {

class IntelLPSSSPIController final : public SPIController {
public:
    IntelLPSSSPIController() : mEngine(mWindow) {}
    ~IntelLPSSSPIController() override;

    // Borrow owner, retain/open PCI and map BAR0. Caller must have verified
    // ownership/wiring and arranged PCI D0. No bus mastering/DMA is enabled.
    IOReturn bind(IOPCIDevice* pci, IOService* owner);
#if defined(KERNEL)
    IOReturn configureChipSelects(IOACPIPlatformDevice* controller, IOService* owner) {
        traceStage(owner, "CHIP_SELECT_BIND_BEGIN");
        const auto result = mChipSelects.bind(controller, owner, traceStage);
        traceStage(owner, "CHIP_SELECT_BIND_END", result);
        if (owner) {
            owner->setProperty("Cirrus_Intel_SPI_GPIO_Bind_Return", uint64_t(uint32_t(result)), 32);
            owner->setProperty("Cirrus_Intel_SPI_CS_Map_Count", uint64_t(mChipSelects.count()), 32);
            for (size_t i = 0; i < mChipSelects.count(); ++i) {
                char key[64]{};
                snprintf(key, sizeof(key), "Cirrus_Intel_SPI_CS_%u_Source", unsigned(i));
                owner->setProperty(key, mChipSelects.usesGPIO(i) ? "GPIO" : "NATIVE_OR_UNRESOLVED");
                snprintf(key, sizeof(key), "Cirrus_Intel_SPI_CS_%u_GPIO_Pin", unsigned(i));
                owner->setProperty(key, uint64_t(mChipSelects.pin(i)), 32);
            }
        }
        return result;
    }
#endif
    const char* bindFailureReason() const { return mBindFailureReason; }
    uint32_t initialCapabilities() const { return mEngine.initialCapabilities(); }
    uint32_t initialStatus() const { return mEngine.initialStatus(); }
    void shutdown();
    void suspend();
    IOReturn resume();
    size_t maxTransferBytes() const override { return LPSSSPIEngine::kMaxFrameBytes; }
    IOReturn exchange(const SPIEndpoint& endpoint, const uint8_t* tx, uint8_t* rx, size_t length) override;

private:
    class MMIOWindow final : public RegisterWindow {
    public:
        volatile uint8_t* base{nullptr};
        IOService* owner{nullptr};
        uint32_t read32(uint32_t offset) override;
        void write32(uint32_t offset, uint32_t value) override;
        uint64_t nowUs() override;
        void stage(const char*, IOReturn = kIOReturnSuccess) override;
        void delayUs(uint32_t delay) override { IODelay(delay); }
    };

    bool pciIsD0() const;
    static void traceStage(IOService* owner, const char* stage, IOReturn result = kIOReturnSuccess);
    uint16_t mDeviceId{0};
    LPSSSPIProfile mProfile{};
    IntelLPSSSPIController(const IntelLPSSSPIController&) = delete;
    IntelLPSSSPIController& operator=(const IntelLPSSSPIController&) = delete;
    MMIOWindow mWindow;
    LPSSSPIEngine mEngine;
#if defined(KERNEL)
    IntelSPIChipSelects mChipSelects;
#endif
    IOPCIDevice* mPCI{nullptr};
    IOService* mOwner{nullptr};
    IOMemoryMap* mMap{nullptr};
    IOLock* mLock{nullptr};
    bool mMemoryWasEnabled{false};
    bool mMemoryChanged{false};
    uint32_t mTracedSelects{0};
    const char* mBindFailureReason{"NONE"};
};

}
