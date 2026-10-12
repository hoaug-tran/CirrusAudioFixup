// AMD AMDI0061/AMDI0062 command/FIFO engine. Register facts reference Linux
// v6.12 drivers/spi/spi-amd.c; no Linux implementation code is incorporated.
// The owner supplies exclusive MMIO access and orders ACPI power transitions.
#pragma once
#include "Transport/SPIController.hpp"
namespace cirrus::transport::amd {
class RegisterWindow {
public:
    virtual ~RegisterWindow() = default;
    virtual uint8_t read8(uint32_t offset) = 0;
    virtual void write8(uint32_t offset, uint8_t value) = 0;
    virtual uint32_t read32(uint32_t offset) = 0;
    virtual void write32(uint32_t offset, uint32_t value) = 0;
    virtual uint64_t nowUs() = 0;
    virtual void delayUs(uint32_t delay) = 0;
};
enum class AMDSPIRevision { V1, V2 };
class AMDSPIEngine final : public SPIController {
public:
    explicit AMDSPIEngine(RegisterWindow& window) : mWindow(window) {}
    static constexpr uint32_t kMinimumWindowBytes = 0xc8;
    IOReturn initialize(AMDSPIRevision revision) {
        if (mInitialized)
            return kIOReturnBusy;
        if (revision != AMDSPIRevision::V1 && revision != AMDSPIRevision::V2)
            return kIOReturnUnsupported;
        mRevision = revision;
        uint32_t status = mWindow.read32(statusOffset());
        if (status == 0xffffffffU)
            return kIOReturnNoDevice;
        if (status & kBusy)
            return kIOReturnBusy;
        mSavedEnable = mWindow.read32(0x20);
        mSavedSpeed = mWindow.read32(0x6c);
        mSavedCS = mWindow.read8(0x1d);
        mInitialized = mOnline = true;
        return kIOReturnSuccess;
    }
    size_t maxTransferBytes() const override { return 64; }
    uint32_t actualClockHz() const { return mActualClockHz; }
    // This hardware provides command TX followed by RX, not full duplex.
    IOReturn exchange(const SPIEndpoint&, const uint8_t*, uint8_t*, size_t) override { return kIOReturnUnsupported; }
    IOReturn command(const SPIEndpoint& ep, const uint8_t* tx, size_t txLength, uint8_t* rx, size_t rxLength) override {
        if (!mInitialized || !mOnline)
            return kIOReturnNotReady;
        if (!tx || !txLength || txLength > 64 || rxLength > 64 - txLength || (rxLength && !rx) || ep.chipSelect > 3 || !ep.clockHz)
            return kIOReturnBadArgument;
        if (ep.mode != 0)
            return kIOReturnUnsupported;
        uint32_t status = mWindow.read32(statusOffset());
        if (status == 0xffffffffU) {
            mOnline = false;
            return kIOReturnNoDevice;
        }
        if (status & kBusy)
            return kIOReturnBusy;
        struct Rate {
            uint32_t hz, selector, divider;
        };
        constexpr Rate rates[] = {{100000000, 4, 0}, {66660000, 0, 0},   {50000000, 7, 4},   {33330000, 1, 0}, {22220000, 2, 0},
                                  {16660000, 3, 0},  {4000000, 7, 0x32}, {3170000, 7, 0x3f}, {800000, 5, 0}};
        const Rate* rate = nullptr;
        for (const auto& candidate : rates)
            if (candidate.hz <= ep.clockHz) {
                rate = &candidate;
                break;
            }
        if (!rate)
            return kIOReturnUnsupported;
        mActualClockHz = rate->hz;
        mTouched = true;
        uint32_t enable = mWindow.read32(0x20);
        // Clear SPI100 when switching back down, preserving other enable bits.
        mWindow.write32(0x20, (enable & ~uint32_t(0xf00001)) | (rate->selector << 20) | (rate->hz == 100000000 ? 1U : 0U));
        if (rate->divider)
            mWindow.write32(0x6c, (mWindow.read32(0x6c) & ~0x3f00U) | (rate->divider << 8));
        mWindow.write8(0x1d, uint8_t((mWindow.read8(0x1d) & ~3U) | ep.chipSelect));
        // First wire byte uses the opcode register; the rest shares FIFO with RX.
        uint32_t control = mWindow.read32(0);
        mWindow.write32(0, control | (1U << 20));
        for (size_t i = 1; i < txLength; ++i)
            mWindow.write8(uint32_t(0x80 + i - 1), tx[i]);
        if (mRevision == AMDSPIRevision::V1)
            mWindow.write32(0, (mWindow.read32(0) & ~0xffU) | tx[0]);
        else
            mWindow.write8(0x45, tx[0]);
        mWindow.write8(0x48, uint8_t(txLength - 1));
        mWindow.write8(0x4b, uint8_t(rxLength));
        uint64_t start = mWindow.nowUs();
        if (mRevision == AMDSPIRevision::V1)
            mWindow.write32(0, mWindow.read32(0) | (1U << 16));
        else
            mWindow.write8(0x47, uint8_t(mWindow.read8(0x47) | 0x80));
        while (true) {
            status = mWindow.read32(statusOffset());
            if (status == 0xffffffffU) {
                mOnline = false;
                return kIOReturnNoDevice;
            }
            if (!(status & kBusy))
                break;
            if (mWindow.nowUs() - start >= 2000000) {
                // Hardware owns CS for the command; do not select another chip
                // or reset a running command. Recovery requires owner power/reset.
                mOnline = false;
                return kIOReturnTimeout;
            }
            mWindow.delayUs(20);
        }
        for (size_t i = 0; i < rxLength; ++i)
            rx[i] = mWindow.read8(uint32_t(0x80 + txLength - 1 + i));
        return kIOReturnSuccess;
    }
    // Owner must wait for command() to finish before suspending/shutting down.
    void suspend() { mOnline = false; }
    IOReturn resume() {
        if (!mInitialized)
            return kIOReturnNotReady;
        uint32_t s = mWindow.read32(statusOffset());
        if (s == 0xffffffffU)
            return kIOReturnNoDevice;
        if (s & kBusy)
            return kIOReturnBusy;
        mOnline = true;
        return kIOReturnSuccess;
    }
    void abandon() { mInitialized = mOnline = mTouched = false; }
    void shutdown() {
        if (mInitialized && mOnline && mTouched) {
            uint32_t s = mWindow.read32(statusOffset());
            if (s != 0xffffffffU && !(s & kBusy)) {
                mWindow.write32(0x20, mSavedEnable);
                mWindow.write32(0x6c, mSavedSpeed);
                mWindow.write8(0x1d, mSavedCS);
            }
        }
        abandon();
    }

private:
    uint32_t statusOffset() const { return mRevision == AMDSPIRevision::V1 ? 0 : 0x4c; }
    static constexpr uint32_t kBusy = 1U << 31;
    RegisterWindow& mWindow;
    AMDSPIRevision mRevision{AMDSPIRevision::V1};
    bool mInitialized{false}, mOnline{false}, mTouched{false};
    uint32_t mSavedEnable{0}, mSavedSpeed{0}, mActualClockHz{0};
    uint8_t mSavedCS{0};
};
}
