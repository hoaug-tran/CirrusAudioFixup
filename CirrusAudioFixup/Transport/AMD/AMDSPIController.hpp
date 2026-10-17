// ACPI MMIO owner for AMD AMDI0061/AMDI0062 SPI command controllers.
// An owning IOService supplies D0 and joins clients before teardown. No match
// personality, amplifier reset, board authorization or AML power policy implied.
#pragma once
#include "AMDSPIEngine.hpp"
#include <IOKit/acpi/IOACPIPlatformDevice.h>
namespace cirrus::transport::amd {
class AMDSPIController final : public SPIController {
public:
    AMDSPIController() : mEngine(mWindow) {}
    ~AMDSPIController() override { shutdown(); }
    static bool supportsProvider(IOACPIPlatformDevice* provider);
    IOReturn bind(IOACPIPlatformDevice* provider, IOService* owner);
    void suspend();
    IOReturn resume();
    void shutdown();
    size_t maxTransferBytes() const override { return 64; }
    IOReturn exchange(const SPIEndpoint&, const uint8_t*, uint8_t*, size_t) override { return kIOReturnUnsupported; }
    IOReturn command(const SPIEndpoint&, const uint8_t*, size_t, uint8_t*, size_t) override;

private:
    class MMIOWindow final : public RegisterWindow {
    public:
        volatile uint8_t* base{nullptr};
        uint8_t read8(uint32_t) override;
        void write8(uint32_t, uint8_t) override;
        uint32_t read32(uint32_t) override;
        void write32(uint32_t, uint32_t) override;
        uint64_t nowUs() override;
        void delayUs(uint32_t us) override { IODelay(us); }
    };
    bool isD0() const;
    static bool resolveRevision(IOACPIPlatformDevice* provider, AMDSPIRevision& revision);
    AMDSPIController(const AMDSPIController&) = delete;
    AMDSPIController& operator=(const AMDSPIController&) = delete;
    MMIOWindow mWindow;
    AMDSPIEngine mEngine;
    IOACPIPlatformDevice* mProvider{nullptr};
    IOService* mOwner{nullptr};
    IOMemoryMap* mMap{nullptr};
    IOLock* mLock{nullptr};
    bool mPowerAvailable{false};
};
}
