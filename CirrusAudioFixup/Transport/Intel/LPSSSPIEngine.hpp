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
    virtual void stage(const char*, IOReturn = kIOReturnSuccess) {}
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
    static constexpr uint32_t kClockEnable = 1U;
    static constexpr uint32_t kClockUpdate = 1U << 31;

    // Caller must hold exclusive ownership of the controller and serialize
    // initialize/exchange/suspend/resume/shutdown, including power transitions.
    explicit LPSSSPIEngine(RegisterWindow& window) : mWindow(window) {}

    uint32_t initialCapabilities() const { return mInitialCapabilities; }
    uint32_t initialStatus() const { return mInitialStatus; }
    uint32_t initialClock() const { return mSavedClock; }
    uint32_t preparedClock() const { return mPreparedClock; }
    uint32_t initialClockGate() const { return mSavedGate; }
    IOReturn initialize(LPSSSPIProfile profile = {LPSSSPIFamily::CannonLake, kRootClockHz, true, true, 0x300}) {
        if (mInitialized)
            return kIOReturnBusy;
        mInitialCapabilities = mInitialStatus = 0;
        mPreparedClock = 0;
        mProfile = profile;
        if (!mProfile.rootClockHz)
            return kIOReturnBadArgument;
        mWindow.stage("PRIVATE_STATE_BEGIN");
        uint32_t caps = mProfile.capabilities ? mWindow.read32(kCapabilities) : 0x220;
        mInitialCapabilities = caps;
        if (caps == 0xffffffffU || ((caps >> 4) & 0xf) != 2) {
            mWindow.stage("PRIVATE_STATE_END", kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        uint32_t selectMask = (caps >> 9) & 0xf;
        // Native CS pins are enabled consecutively from CS0. Do not invent
        // GPIO chip selects or silently select a disabled output.
        if (!selectMask || (selectMask & (selectMask + 1))) {
            mWindow.stage("PRIVATE_STATE_END", kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        mSelectCount = 0;
        for (uint32_t bits = selectMask; bits; bits >>= 1)
            ++mSelectCount;
        // LPSS private registers remain separate from the clocked SSP block.
        // Linux intel-lpss prepares reset, then spi-pxa2xx enables its clock
        // before touching SSP registers. Preserve the independently owned DMA
        // reset and fractional clock fields; never reset the whole function.
        mSavedSelect = mWindow.read32(kChipSelect);
        mSavedClock = mWindow.read32(kClock);
        mSavedGate = mWindow.read32(kClockGate);
        mSavedReset = mWindow.read32(kReset);
        if (mSavedSelect == 0xffffffffU || mSavedClock == 0xffffffffU || mSavedGate == 0xffffffffU || mSavedReset == 0xffffffffU) {
            mWindow.stage("PRIVATE_STATE_END", kIOReturnNoDevice);
            return kIOReturnNoDevice;
        }
        mWindow.stage("PRIVATE_STATE_END");
        mWindow.stage("FUNCTION_PREPARE_BEGIN");
        const auto prepared = prepareHardware();
        mWindow.stage("FUNCTION_PREPARE_END", prepared);
        if (prepared != kIOReturnSuccess) {
            restorePrerequisites();
            return prepared;
        }
        mWindow.stage("SSP_CONTEXT_BEGIN");
        const uint32_t status = mWindow.read32(kStatus);
        mInitialStatus = status;
        if (status == 0xffffffffU || (status & kBusy)) {
            mWindow.stage("SSP_CONTEXT_END", status == 0xffffffffU ? kIOReturnNoDevice : kIOReturnBusy);
            restorePrerequisites();
            return status == 0xffffffffU ? kIOReturnNoDevice : kIOReturnBusy;
        }
        mSavedControl0 = mWindow.read32(kControl0);
        mSavedControl1 = mWindow.read32(kControl1);
        mSavedTimeout = mWindow.read32(0x28);
        mSavedProtocol = mWindow.read32(0x2c);
        if (mSavedControl0 == 0xffffffffU || mSavedControl1 == 0xffffffffU || mSavedTimeout == 0xffffffffU || mSavedProtocol == 0xffffffffU) {
            mWindow.stage("SSP_CONTEXT_END", kIOReturnNoDevice);
            restorePrerequisites();
            return kIOReturnNoDevice;
        }
        mWindow.stage("SSP_CONTEXT_END");
        mInitialized = true;
        mOnline = true;
        return kIOReturnSuccess;
    }

    uint32_t selectCount() const { return mSelectCount; }
    uint32_t actualClockHz() const { return mActualClockHz; }
    bool online() const { return mInitialized && mOnline; }

    void suspend() {
        if (online() && mTouched && hardwareReady())
            quiesce();
        mOnline = false;
    }

    void markOffline() { mOnline = false; }
    void abandon() {
        mInitialized = mOnline = mTouched = false;
        mClockChanged = mResetChanged = false;
        mSelectCount = 0;
    }

    IOReturn resume() {
        if (!mInitialized)
            return kIOReturnNotReady;
        mOnline = false;
        uint32_t caps = mProfile.capabilities ? mWindow.read32(kCapabilities) : 0x220;
        if (caps == 0xffffffffU || ((caps >> 4) & 0xf) != 2 || ((caps >> 9) & 0xf) != ((1U << mSelectCount) - 1))
            return kIOReturnUnsupported;
        mWindow.stage("RESUME_FUNCTION_PREPARE_BEGIN");
        const auto prepared = prepareHardware();
        mWindow.stage("RESUME_FUNCTION_PREPARE_END", prepared);
        if (prepared != kIOReturnSuccess)
            return prepared;
        mOnline = true;
        return kIOReturnSuccess;
    }

    // Must run in D0 after clients have stopped. Leave the SSP disabled and
    // CS inactive; restoring an old enabled SSP could restart stale traffic.
    void shutdown() {
        if (online() && mTouched && hardwareReady()) {
            quiesce();
            mWindow.write32(kControl1, mSavedControl1);
            mWindow.write32(0x28, mSavedTimeout);
            mWindow.write32(0x2c, mSavedProtocol);
            mWindow.write32(kControl0, mSavedControl0 & ~kEnabled);
            mWindow.write32(kChipSelect, mSavedSelect | 3U);
            mWindow.write32(kClockGate, mSavedGate);
        }
        restorePrerequisites();
        mInitialized = mOnline = mTouched = false;
    }

    IOReturn exchange(const SPIEndpoint& endpoint, const uint8_t* tx, uint8_t* rx, size_t length, SPIChipSelect* external = nullptr) {
        if (!online())
            return kIOReturnNotReady;
        if (!tx || !length || length > kMaxFrameBytes || !endpoint.clockHz || endpoint.mode > 3 ||
            (!external && endpoint.chipSelect >= mSelectCount))
            return kIOReturnBadArgument;
        // Power/context loss must go through resume. Do not read SSP first,
        // nor attempt to repair reset/clock in the middle of a transfer.
        if (!hardwareReady()) {
            markOffline();
            return kIOReturnNotReady;
        }
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
        // The elapsed-time deadline is primary. Independently bound completed
        // polling iterations as well, so a stalled clock cannot spin forever.
        // Progress may omit a delay, hence two iterations per transmitted byte.
        uint64_t pollsRemaining = budgetUs + uint64_t(length) * 2 + 256;

        mTouched = true;
        uint32_t gate = mWindow.read32(kClockGate);
        if (mProfile.forceClockGate)
            mWindow.write32(kClockGate, (gate & ~3U) | 3U);
        mWindow.write32(kControl0, mWindow.read32(kControl0) & ~kEnabled);
        uint32_t inactive = (mWindow.read32(kChipSelect) & ~mProfile.selectMask) | (external ? 0 : endpoint.chipSelect << 8) | 3U;
        mWindow.write32(kChipSelect, inactive);
        // CS selection is latched; wait at least two input clock cycles before
        // assertion, allowing a slow fractional input clock as well.
        uint64_t selectDelay = (2000000ULL * denominator + clockNumerator - 1) / clockNumerator;
        mWindow.delayUs(static_cast<uint32_t>(selectDelay ? selectDelay : 1));
        const auto drained = drainRx();
        if (drained != kIOReturnSuccess)
            return finish(drained, inactive, gate, nullptr, drained == kIOReturnNoDevice);
        mWindow.write32(kStatus, kOverrun | kTimeoutStatus | kBitError);
        // Motorola master, eight bits, no IRQ or DMA service requests.
        uint32_t cr0 = (static_cast<uint32_t>(divisor - 1) << 8) | 7U;
        uint32_t cr1 = ((endpoint.mode & 2) ? (1U << 3) : 0) | ((endpoint.mode & 1) ? (1U << 4) : 0);
        mWindow.write32(kControl1, cr1);
        mWindow.write32(0x28, 0);
        mWindow.write32(0x2c, 0);
        mWindow.write32(kControl0, cr0 | kEnabled);
        if (external) {
            const auto result = external->setActive(true);
            if (result != kIOReturnSuccess)
                return finish(result, inactive, gate, external);
        } else
            mWindow.write32(kChipSelect, inactive & ~2U);

        size_t sent = 0, received = 0;
        while (received < length) {
            if (!pollsRemaining--)
                return finish(kIOReturnTimeout, inactive, gate, external);
            size_t previousSent = sent, previousReceived = received;
            status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return finish(kIOReturnNoDevice, inactive, gate, external, true);
            if (status & (kOverrun | kBitError))
                return finish(kIOReturnIOError, inactive, gate, external);
            // Drain receive data even on write-only calls. Keep at most eight
            // bytes outstanding, below the CNL FIFO depth, to avoid overrun.
            if (status & kRxReady) {
                if (received == sent)
                    return finish(kIOReturnIOError, inactive, gate, external);
                uint8_t value = static_cast<uint8_t>(mWindow.read32(kData));
                if (rx)
                    rx[received] = value;
                ++received;
            }
            if (sent < length && sent - received < 8 && (status & kTxReady))
                mWindow.write32(kData, tx[sent++]);
            if (mWindow.nowUs() - started >= budgetUs)
                return finish(kIOReturnTimeout, inactive, gate, external);
            // Short polling waits preserve CS. No sleeping allocator or
            // interrupt completion can outlive the borrowed buffers.
            if (sent == previousSent && received == previousReceived)
                mWindow.delayUs(1);
        }
        do {
            if (!pollsRemaining--)
                return finish(kIOReturnTimeout, inactive, gate, external);
            status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return finish(kIOReturnNoDevice, inactive, gate, external, true);
            if (status & (kOverrun | kBitError))
                return finish(kIOReturnIOError, inactive, gate, external);
            if (!(status & kBusy))
                break;
            if (mWindow.nowUs() - started >= budgetUs)
                return finish(kIOReturnTimeout, inactive, gate, external);
            mWindow.delayUs(1);
        } while (true);
        return finish(kIOReturnSuccess, inactive, gate, external);
    }

private:
    bool hardwareReady() {
        const uint32_t reset = mWindow.read32(kReset), clock = mWindow.read32(kClock);
        return reset != 0xffffffffU && clock != 0xffffffffU && (reset & 3U) == 3U && (clock & 1U);
    }
    IOReturn prepareHardware() {
        const uint32_t reset = mWindow.read32(kReset), clock = mWindow.read32(kClock);
        if (reset == 0xffffffffU || clock == 0xffffffffU)
            return kIOReturnNoDevice;
        if ((reset & 3U) != 3U) {
            mResetChanged = true;
            mWindow.write32(kReset, reset | 3U);
            if ((mWindow.read32(kReset) & 3U) != 3U)
                return kIOReturnNotReady;
        }
        if ((clock & (kClockEnable | kClockUpdate)) != (kClockEnable | kClockUpdate)) {
            mClockChanged = true;
            mWindow.write32(kClock, clock | kClockEnable | kClockUpdate);
        }
        mPreparedClock = mWindow.read32(kClock);
        if (mPreparedClock == 0xffffffffU || !(mPreparedClock & kClockEnable) ||
            (mPreparedClock & 0x7ffffffeU) != (clock & 0x7ffffffeU))
            return kIOReturnNotReady;
        return hardwareReady() ? kIOReturnSuccess : kIOReturnNotReady;
    }
    void restorePrerequisites() {
        if ((mClockChanged || mResetChanged) && mWindow.read32(kClock) != 0xffffffffU && mWindow.read32(kReset) != 0xffffffffU) {
            if (mClockChanged)
                mWindow.write32(kClock, mSavedClock);
            if (mResetChanged)
                mWindow.write32(kReset, mSavedReset);
        }
        mClockChanged = mResetChanged = false;
    }
    IOReturn drainRx() {
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t status = mWindow.read32(kStatus);
            if (status == 0xffffffffU)
                return kIOReturnNoDevice;
            if (!(status & kRxReady))
                return kIOReturnSuccess;
            (void)mWindow.read32(kData);
        }
        return kIOReturnIOError;
    }

    IOReturn finish(IOReturn result, uint32_t inactive, uint32_t gate, SPIChipSelect* external = nullptr, bool controllerGone = false) {
        uint32_t control = controllerGone ? 0xffffffffU : mWindow.read32(kControl0);
        if (controllerGone || control == 0xffffffffU) {
            mOnline = false;
            if (external)
                (void)external->setActive(false);
            return kIOReturnNoDevice;
        }
        // Stop clocks on error before releasing CS. On success BSY was already
        // clear. Force the clock gate on while changing CS, a CNL LPSS quirk.
        mWindow.write32(kControl0, control & ~kEnabled);
        mWindow.write32(kChipSelect, inactive);
        (void)mWindow.read32(kChipSelect);
        if (external) {
            const auto released = external->setActive(false);
            if (released != kIOReturnSuccess) {
                mOnline = false;
                if (result == kIOReturnSuccess)
                    result = released;
            }
        }
        if (drainRx() == kIOReturnNoDevice) {
            mOnline = false;
            return result == kIOReturnSuccess ? kIOReturnNoDevice : result;
        }
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

    uint32_t mInitialCapabilities{0}, mInitialStatus{0};
    LPSSSPIProfile mProfile{LPSSSPIFamily::CannonLake, kRootClockHz, true, true, 0x300};
    RegisterWindow& mWindow;
    bool mInitialized{false}, mOnline{false}, mTouched{false};
    bool mClockChanged{false}, mResetChanged{false};
    uint32_t mSelectCount{0}, mActualClockHz{0};
    uint32_t mSavedControl0{0}, mSavedControl1{0}, mSavedSelect{0};
    uint32_t mSavedClock{0}, mSavedGate{0}, mSavedReset{0};
    uint32_t mPreparedClock{0};
    uint32_t mSavedTimeout{0}, mSavedProtocol{0};
};

}
