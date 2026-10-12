// Intel SPT/BXT/CNL-family LPSS SSP register engine, with polling rather than DMA/IRQ.
// Register facts and quirks checked against Linux v6.12 intel-lpss{,-pci}.c,
// spi-pxa2xx.c and pxa2xx_ssp.h (Intel/PXA driver authors). This implementation
// uses an injected register window so FIFO behavior can be tested on the host.
#pragma once

#include "LPSSSPIProfiles.hpp"
#include "Transport/SPIController.hpp"

namespace cirrus::transport::intel {

class RegisterWindow {
public:
    virtual ~RegisterWindow() = default;
    virtual uint32_t read32(uint32_t offset) = 0;
    virtual void write32(uint32_t offset, uint32_t value) = 0;
    virtual uint64_t nowUs() = 0;
    virtual void delayUs(uint32_t delay) = 0;
};

class LPSSSPIEngine {
public:
    static constexpr size_t kMaxFrameBytes = 4096;
    static constexpr uint32_t kRootClockHz = 100000000;
    static constexpr uint32_t kMinimumWindowBytes = 0x300;
    static constexpr uint32_t kControl0 = 0x00;
    static constexpr uint32_t kControl1 = 0x04;
    static constexpr uint32_t kStatus = 0x08;
    static constexpr uint32_t kData = 0x10;
    static constexpr uint32_t kClock = 0x200;
    static constexpr uint32_t kReset = 0x204;
    static constexpr uint32_t kChipSelect = 0x224;
    static constexpr uint32_t kClockGate = 0x238;
    static constexpr uint32_t kCapabilities = 0x2fc;
    static constexpr uint32_t kEnabled = 1U << 7;
    static constexpr uint32_t kTxReady = 1U << 2;
    static constexpr uint32_t kRxReady = 1U << 3;
    static constexpr uint32_t kBusy = 1U << 4;
    static constexpr uint32_t kOverrun = 1U << 7;
    static constexpr uint32_t kTimeoutStatus = 1U << 19;
    static constexpr uint32_t kBitError = 1U << 23;

    // Caller must hold exclusive ownership of the controller and serialize
    // initialize/exchange/suspend/resume/shutdown, including power transitions.
    explicit LPSSSPIEngine(RegisterWindow& window) : mWindow(window) {}

    IOReturn initialize(LPSSSPIProfile profile = {LPSSSPIFamily::CannonLake, kRootClockHz, true, true, 0x300}) {
        if (mInitialized)
            return kIOReturnBusy;
        mProfile = profile;
        if (!mProfile.rootClockHz)
            return kIOReturnBadArgument;
        uint32_t caps = mProfile.capabilities ? mWindow.read32(kCapabilities) : 0x220;
        uint32_t status = mWindow.read32(kStatus);
        if (caps == 0xffffffffU || status == 0xffffffffU || ((caps >> 4) & 0xf) != 2)
            return kIOReturnUnsupported;
        if (status & kBusy)
            return kIOReturnBusy;
        uint32_t selectMask = (caps >> 9) & 0xf;
        // Native CS pins are enabled consecutively from CS0. Do not invent
        // GPIO chip selects or silently select a disabled output.
        if (!selectMask || (selectMask & (selectMask + 1)))
            return kIOReturnUnsupported;
        mSelectCount = 0;
        for (uint32_t bits = selectMask; bits; bits >>= 1)
            ++mSelectCount;
        mSavedControl0 = mWindow.read32(kControl0);
        mSavedControl1 = mWindow.read32(kControl1);
        mSavedSelect = mWindow.read32(kChipSelect);
        mSavedClock = mWindow.read32(kClock);
        mSavedGate = mWindow.read32(kClockGate);
        mSavedReset = mWindow.read32(kReset);
        mSavedTimeout = mWindow.read32(0x28);
        mSavedProtocol = mWindow.read32(0x2c);
        // Capture hardware state before any write. DMA remains untouched.
        mInitialized = true;
        mOnline = true;
        return kIOReturnSuccess;
    }

    uint32_t selectCount() const { return mSelectCount; }
    uint32_t actualClockHz() const { return mActualClockHz; }
    bool online() const { return mInitialized && mOnline; }

    void suspend() {
        if (online() && mTouched)
            quiesce();
        mOnline = false;
    }

    void markOffline() { mOnline = false; }
    void abandon() {
        mInitialized = mOnline = mTouched = false;
        mSelectCount = 0;
    }

    IOReturn resume() {
        if (!mInitialized)
            return kIOReturnNotReady;
        uint32_t caps = mProfile.capabilities ? mWindow.read32(kCapabilities) : 0x220;
        if (caps == 0xffffffffU || ((caps >> 4) & 0xf) != 2 || ((caps >> 9) & 0xf) != ((1U << mSelectCount) - 1))
            return kIOReturnUnsupported;
        mOnline = true;
        return kIOReturnSuccess;
    }

    // Must run in D0 after clients have stopped. Leave the SSP disabled and
    // CS inactive; restoring an old enabled SSP could restart stale traffic.
    void shutdown() {
        if (online() && mTouched) {
            quiesce();
            mWindow.write32(kControl1, mSavedControl1);
            mWindow.write32(0x28, mSavedTimeout);
            mWindow.write32(0x2c, mSavedProtocol);
            mWindow.write32(kControl0, mSavedControl0 & ~kEnabled);
            mWindow.write32(kChipSelect, mSavedSelect | 3U);
            mWindow.write32(kClock, mSavedClock);
            mWindow.write32(kClockGate, mSavedGate);
            mWindow.write32(kReset, mSavedReset);
        }
        mInitialized = mOnline = mTouched = false;
    }

    IOReturn exchange(const SPIEndpoint& endpoint, const uint8_t* tx, uint8_t* rx, size_t length) {
        if (!online())
            return kIOReturnNotReady;
        if (!tx || !length || length > kMaxFrameBytes || !endpoint.clockHz || endpoint.mode > 3 || endpoint.chipSelect >= mSelectCount)
            return kIOReturnBadArgument;
        uint32_t status = mWindow.read32(kStatus);
        if (status == 0xffffffffU) {
            markOffline();
            return kIOReturnNoDevice;
        }
        if (status & kBusy)
            return kIOReturnBusy;

        // Account for the firmware-programmed fractional LPSS clock rather
        // than assuming root clock equals the actual SSP input clock.
        uint32_t clock = mWindow.read32(kClock);
        uint32_t numerator = (clock >> 1) & 0x7fff;
        uint32_t denominator = (clock >> 16) & 0x7fff;
        if (!numerator && !denominator)
            numerator = denominator = 1;
        if (!numerator || !denominator || numerator > denominator)
            return kIOReturnUnsupported;
        uint64_t clockNumerator = uint64_t(mProfile.rootClockHz) * numerator;
        uint64_t divisorDenominator = uint64_t(denominator) * endpoint.clockHz;
        uint64_t divisor = (clockNumerator + divisorDenominator - 1) / divisorDenominator;
        if (!divisor || divisor > 4096)
            return kIOReturnUnsupported;
        uint64_t actualDenominator = uint64_t(denominator) * divisor;
        mActualClockHz = static_cast<uint32_t>(clockNumerator / actualDenominator);
        uint64_t bitCount = uint64_t(length) * 8;
        uint64_t wireUs = (bitCount * actualDenominator * 1000000ULL + clockNumerator - 1) / clockNumerator;
        // Bound CPU occupancy and reject a frame that cannot fit the budget.
        // The register adapter can submit smaller chunks for slow endpoints.
        if (wireUs > 200000)
            return kIOReturnUnsupported;
        uint64_t budgetUs = wireUs * 2 + 10000;
        uint64_t started = mWindow.nowUs();

        mTouched = true;
        // Deassert function reset only, preserving the independent DMA reset.
        mWindow.write32(kReset, mWindow.read32(kReset) | 3U);
        mWindow.write32(kClock, clock | 1U);
        uint32_t gate = mWindow.read32(kClockGate);
        if (mProfile.forceClockGate)
            mWindow.write32(kClockGate, (gate & ~3U) | 3U);
        mWindow.write32(kControl0, mWindow.read32(kControl0) & ~kEnabled);
        uint32_t inactive = (mWindow.read32(kChipSelect) & ~mProfile.selectMask) | (endpoint.chipSelect << 8) | 3U;
        mWindow.write32(kChipSelect, inactive);
        // CS selection is latched; wait at least two input clock cycles before
        // assertion, allowing a slow fractional input clock as well.
        uint64_t selectDelay = (2000000ULL * denominator + clockNumerator - 1) / clockNumerator;
        mWindow.delayUs(static_cast<uint32_t>(selectDelay ? selectDelay : 1));
        if (!drainRx())
            return finish(kIOReturnIOError, inactive, gate);
        mWindow.write32(kStatus, kOverrun | kTimeoutStatus | kBitError);
        // Motorola master, eight bits, no IRQ or DMA service requests.
        uint32_t cr0 = (static_cast<uint32_t>(divisor - 1) << 8) | 7U;
        uint32_t cr1 = ((endpoint.mode & 2) ? (1U << 3) : 0) | ((endpoint.mode & 1) ? (1U << 4) : 0);
        mWindow.write32(kControl1, cr1);
        mWindow.write32(0x28, 0);
        mWindow.write32(0x2c, 0);
        mWindow.write32(kControl0, cr0 | kEnabled);
        mWindow.write32(kChipSelect, inactive & ~2U);

        size_t sent = 0, received = 0;
        while (received < length) {
            size_t previousSent = sent, previousReceived = received;
            status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return finish(kIOReturnNoDevice, inactive, gate);
            if (status & (kOverrun | kBitError))
                return finish(kIOReturnIOError, inactive, gate);
            // Drain receive data even on write-only calls. Keep at most eight
            // bytes outstanding, below the CNL FIFO depth, to avoid overrun.
            if (status & kRxReady) {
                if (received == sent)
                    return finish(kIOReturnIOError, inactive, gate);
                uint8_t value = static_cast<uint8_t>(mWindow.read32(kData));
                if (rx)
                    rx[received] = value;
                ++received;
            }
            if (sent < length && sent - received < 8 && (status & kTxReady))
                mWindow.write32(kData, tx[sent++]);
            if (mWindow.nowUs() - started >= budgetUs)
                return finish(kIOReturnTimeout, inactive, gate);
            // Short polling waits preserve CS. No sleeping allocator or
            // interrupt completion can outlive the borrowed buffers.
            if (sent == previousSent && received == previousReceived)
                mWindow.delayUs(1);
        }
        do {
            status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return finish(kIOReturnNoDevice, inactive, gate);
            if (status & (kOverrun | kBitError))
                return finish(kIOReturnIOError, inactive, gate);
            if (!(status & kBusy))
                break;
            if (mWindow.nowUs() - started >= budgetUs)
                return finish(kIOReturnTimeout, inactive, gate);
            mWindow.delayUs(1);
        } while (true);
        return finish(kIOReturnSuccess, inactive, gate);
    }

private:
    bool drainRx() {
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return false;
            if (!(status & kRxReady))
                return true;
            (void)mWindow.read32(kData);
        }
        return false;
    }

    IOReturn finish(IOReturn result, uint32_t inactive, uint32_t gate) {
        if (result == kIOReturnNoDevice) {
            mOnline = false;
            return result;
        }
        // Stop clocks on error before releasing CS. On success BSY was already
        // clear. Force the clock gate on while changing CS, a CNL LPSS quirk.
        mWindow.write32(kControl0, mWindow.read32(kControl0) & ~kEnabled);
        mWindow.write32(kChipSelect, inactive);
        (void)mWindow.read32(kChipSelect);
        (void)drainRx();
        mWindow.write32(kStatus, kOverrun | kTimeoutStatus | kBitError);
        mWindow.write32(kClockGate, gate);
        return result;
    }

    void quiesce() {
        uint32_t gate = mWindow.read32(kClockGate);
        if (mProfile.forceClockGate)
            mWindow.write32(kClockGate, (gate & ~3U) | 3U);
        mWindow.write32(kControl0, mWindow.read32(kControl0) & ~kEnabled);
        mWindow.write32(kChipSelect, mWindow.read32(kChipSelect) | 3U);
        (void)mWindow.read32(kChipSelect);
        (void)drainRx();
        mWindow.write32(kClockGate, gate);
    }

    LPSSSPIProfile mProfile{LPSSSPIFamily::CannonLake, kRootClockHz, true, true, 0x300};
    RegisterWindow& mWindow;
    bool mInitialized{false}, mOnline{false}, mTouched{false};
    uint32_t mSelectCount{0}, mActualClockHz{0};
    uint32_t mSavedControl0{0}, mSavedControl1{0}, mSavedSelect{0};
    uint32_t mSavedClock{0}, mSavedGate{0}, mSavedReset{0};
    uint32_t mSavedTimeout{0}, mSavedProtocol{0};
};

}
