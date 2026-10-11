// PCI/MMIO owner for the 8086:7AAA Intel LPSS SPI controller.
// The enclosing IOService supplies PCI D0 and quiesces clients before teardown.
// No automatic PCI/ACPI matching or amplifier board profile is implied.
#pragma once

#include "Transport/Intel/LPSSSPIEngine.hpp"
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace cirrus::transport::intel {

class IntelLPSSSPIController final : public SPIController {
public:
    IntelLPSSSPIController() : mEngine(mWindow) {}
    ~IntelLPSSSPIController() override;

    // Borrow owner, retain/open PCI and map BAR0. Caller must have verified
    // ownership/wiring and arranged PCI D0. No bus mastering/DMA is enabled.
    IOReturn bind(IOPCIDevice* pci, IOService* owner);
    void shutdown();
    void suspend();
    IOReturn resume();
    size_t maxTransferBytes() const override { return LPSSSPIEngine::kMaxFrameBytes; }
    IOReturn exchange(const SPIEndpoint& endpoint, const uint8_t* tx,
                      uint8_t* rx, size_t length) override;

private:
    class MMIOWindow final : public RegisterWindow {
    public:
        volatile uint8_t* base{nullptr};
        uint32_t read32(uint32_t offset) override;
        void write32(uint32_t offset, uint32_t value) override;
        uint64_t nowUs() override;
        void delayUs(uint32_t delay) override { IODelay(delay); }
    };

    bool pciIsD0() const;
    IntelLPSSSPIController(const IntelLPSSSPIController&) = delete;
    IntelLPSSSPIController& operator=(const IntelLPSSSPIController&) = delete;
    MMIOWindow mWindow;
    LPSSSPIEngine mEngine;
    IOPCIDevice* mPCI{nullptr};
    IOService* mOwner{nullptr};
    IOMemoryMap* mMap{nullptr};
    IOLock* mLock{nullptr};
    bool mMemoryWasEnabled{false};
    bool mMemoryChanged{false};
};

}
