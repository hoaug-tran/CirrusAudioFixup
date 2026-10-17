// Intel LPSS native-CS PIO transport. Hardware register behavior is described
// by the references in LPSSSPIEngine.hpp; no Linux driver implementation is
// included. Apple private SPI classes are not called or assumed ABI-compatible.
#include "IntelLPSSSPIController.hpp"
#include <kern/clock.h>
#include <mach/mach_time.h>
#if defined(KERNEL)
#include "Support/Logging.hpp"
#endif

namespace cirrus::transport::intel {
void IntelLPSSSPIController::traceStage(IOService* owner, const char* stage, IOReturn result) {
#if defined(KERNEL)
    if (gCirrusDebug) {
        if (owner) {
            owner->setProperty("Cirrus_Intel_SPI_Stage", stage);
            owner->setProperty("Cirrus_Intel_SPI_Stage_Return", uint64_t(uint32_t(result)), 32);
        }
        CIRRUS_LOG("BOOT_FLOW Intel_SPI owner=%llu %s ret=0x%08X",
                   static_cast<unsigned long long>(owner ? owner->getRegistryEntryID() : 0), stage, uint32_t(result));
    }
#else
    (void)owner;
    (void)stage;
    (void)result;
#endif
}

uint32_t IntelLPSSSPIController::MMIOWindow::read32(uint32_t offset) {
    uint32_t value = *reinterpret_cast<volatile uint32_t*>(base + offset);
    OSSynchronizeIO();
    return value;
}

void IntelLPSSSPIController::MMIOWindow::write32(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
    OSSynchronizeIO();
}

uint64_t IntelLPSSSPIController::MMIOWindow::nowUs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

IntelLPSSSPIController::~IntelLPSSSPIController() { shutdown(); }

bool IntelLPSSSPIController::pciIsD0() const {
    if (!mPCI || mPCI->configRead16(0) != 0x8086 || mPCI->configRead16(2) != mDeviceId)
        return false;
    uint8_t offset = 0;
    // PMCSR is four bytes after the PM capability header. An absent PM
    // capability means there is no PCI D-state register to inspect.
    (void)mPCI->findPCICapability(kIOPCIPowerManagementCapability, &offset);
    return !offset || (offset <= 0xf8 && (mPCI->configRead16(static_cast<uint8_t>(offset + 4)) & 3U) == 0);
}

IOReturn IntelLPSSSPIController::bind(IOPCIDevice* pci, IOService* owner) {
    mBindFailureReason = "NONE";
    auto reject = [this](const char* reason, IOReturn result) {
        mBindFailureReason = reason;
        traceStage(mOwner, reason, result);
        return result;
    };
    if (mPCI || mLock)
        return reject("INTEL_CONTROLLER_ALREADY_BOUND", kIOReturnBusy);
    if (!pci || !owner)
        return reject("INTEL_CONTROLLER_ARGUMENT_REJECTED", kIOReturnBadArgument);
    traceStage(owner, "PCI_IDENTITY_BEGIN");
    LPSSSPIProfile profile{};
    if (!lookupLPSSSPIProfile(pci->configRead16(0), pci->configRead16(2), profile))
        return reject("INTEL_CONTROLLER_ID_UNSUPPORTED", kIOReturnUnsupported);
    mDeviceId = pci->configRead16(2);
    mProfile = profile;
    traceStage(owner, "PCI_IDENTITY_END");
    mLock = IOLockAlloc();
    if (!mLock)
        return reject("INTEL_CONTROLLER_LOCK_ALLOCATION_FAILED", kIOReturnNoMemory);
    mPCI = pci;
    mPCI->retain();
    mOwner = owner;
    // Refuse controllers already opened by another client; a second SPI
    // owner could corrupt transfers even if our own lock is held.
    traceStage(owner, "PCI_OPEN_BEGIN");
    if (mPCI->isOpen() || !mPCI->open(mOwner)) {
        mPCI->release();
        mPCI = nullptr;
        mOwner = nullptr;
        IOLockFree(mLock);
        mLock = nullptr;
        return reject("INTEL_CONTROLLER_EXCLUSIVE_OPEN_FAILED", kIOReturnExclusiveAccess);
    }
    traceStage(owner, "PCI_OPEN_END");
#if defined(KERNEL)
    if (gCirrusDebug) {
        uint8_t pmOffset = 0;
        (void)mPCI->findPCICapability(kIOPCIPowerManagementCapability, &pmOffset);
        owner->setProperty("Cirrus_Intel_SPI_PM_Capability_Offset", uint64_t(pmOffset), 32);
        if (pmOffset && pmOffset <= 0xf8)
            owner->setProperty("Cirrus_Intel_SPI_PMCSR", uint64_t(mPCI->configRead16(uint8_t(pmOffset + 4))), 32);
        owner->setProperty("Cirrus_Intel_SPI_PCI_Command", uint64_t(mPCI->configRead16(kIOPCIConfigCommand)), 32);
    }
#endif
    if (!pciIsD0()) {
        shutdown();
        return reject("INTEL_CONTROLLER_NOT_D0", kIOReturnNotReady);
    }
    mMemoryWasEnabled = (mPCI->configRead16(kIOPCIConfigCommand) & 2U) != 0;
    mPCI->setMemoryEnable(true);
    mMemoryChanged = true;
    if (!(mPCI->configRead16(kIOPCIConfigCommand) & 2U)) {
        shutdown();
        return reject("INTEL_CONTROLLER_MEMORY_ENABLE_FAILED", kIOReturnNotReady);
    }
    traceStage(owner, "BAR_MAP_BEGIN");
    mMap = mPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    traceStage(owner, "BAR_MAP_END", mMap ? kIOReturnSuccess : kIOReturnNoResources);
#if defined(KERNEL)
    if (gCirrusDebug && mMap)
        owner->setProperty("Cirrus_Intel_SPI_BAR0_Length", uint64_t(mMap->getLength()), 64);
#endif
    if (!mMap || mMap->getLength() < LPSSSPIEngine::kMinimumWindowBytes || !mMap->getVirtualAddress()) {
        const char* reason = !mMap                                                    ? "INTEL_CONTROLLER_BAR_MAPPING_FAILED"
                             : mMap->getLength() < LPSSSPIEngine::kMinimumWindowBytes ? "INTEL_CONTROLLER_BAR_TOO_SMALL"
                                                                                      : "INTEL_CONTROLLER_BAR_ADDRESS_UNAVAILABLE";
        shutdown();
        return reject(reason, kIOReturnNoResources);
    }
    mWindow.base = reinterpret_cast<volatile uint8_t*>(mMap->getVirtualAddress());
    traceStage(owner, "MMIO_INITIALIZE_BEGIN");
    IOReturn result = mEngine.initialize(mProfile);
    traceStage(owner, "MMIO_INITIALIZE_END", result);
    if (result != kIOReturnSuccess) {
        shutdown();
        return reject("INTEL_CONTROLLER_ENGINE_INITIALIZATION_FAILED", result);
    }
    return result;
}

IOReturn IntelLPSSSPIController::exchange(const SPIEndpoint& endpoint, const uint8_t* tx, uint8_t* rx, size_t length) {
    if (!mLock)
        return kIOReturnNotReady;
    const uint32_t selectBit = endpoint.chipSelect < 32 ? 1U << endpoint.chipSelect : 0;
    const bool trace = selectBit && !(mTracedSelects & selectBit);
    if (trace) {
        mTracedSelects |= selectBit;
        traceStage(mOwner, "FIRST_TRANSFER_BEGIN");
#if defined(KERNEL)
        CIRRUS_LOG("BOOT_FLOW Intel_SPI owner=%llu endpoint cs=%u mode=%u hz=%u bytes=%lu",
                   static_cast<unsigned long long>(mOwner ? mOwner->getRegistryEntryID() : 0), endpoint.chipSelect, endpoint.mode,
                   endpoint.clockHz, length);
#endif
    }
#if defined(KERNEL)
    // Lifecycle is serialized by the service owner. Never enter AML while
    // holding the transaction lock, including on read-only identity probes.
    const auto powerResult = mChipSelects.validatePower();
    if (powerResult != kIOReturnSuccess) {
        if (trace)
            traceStage(mOwner, "FIRST_TRANSFER_POWER_FAILED", powerResult);
        return powerResult;
    }
    if (trace)
        traceStage(mOwner, "FIRST_TRANSFER_POWER_END");
#endif
    if (trace)
        traceStage(mOwner, "FIRST_TRANSFER_LOCK_BEGIN");
    IOLockLock(mLock);
#if defined(KERNEL)
    if (trace)
        CIRRUS_LOG("BOOT_FLOW Intel_SPI owner=%llu FIRST_TRANSFER_LOCK_END",
                   static_cast<unsigned long long>(mOwner ? mOwner->getRegistryEntryID() : 0));
#endif
    IOReturn result = kIOReturnNotReady;
    if (mWindow.base && pciIsD0() && (mPCI->configRead16(kIOPCIConfigCommand) & 2U)) {
#if defined(KERNEL)
        result = mChipSelects.validateOutputs();
        if (result == kIOReturnSuccess)
            result = mEngine.exchange(endpoint, tx, rx, length, mChipSelects.select(endpoint.chipSelect));
#else
        result = mEngine.exchange(endpoint, tx, rx, length);
#endif
    }
    IOLockUnlock(mLock);
    if (trace)
        traceStage(mOwner, "FIRST_TRANSFER_END", result);
    return result;
}

void IntelLPSSSPIController::suspend() {
    if (!mLock)
        return;
    IOLockLock(mLock);
    // The owner calls this before PCI D3. If PCI is already inaccessible,
    // only mark offline: never issue MMIO against a powered-down function.
    if (mWindow.base && pciIsD0() && (mPCI->configRead16(kIOPCIConfigCommand) & 2U))
        mEngine.suspend();
    else
        mEngine.markOffline();
    IOLockUnlock(mLock);
}

IOReturn IntelLPSSSPIController::resume() {
    if (!mLock)
        return kIOReturnNotReady;
    traceStage(mOwner, "RESUME_BEGIN");
#if defined(KERNEL)
    const auto powerResult = mChipSelects.validatePower();
    if (powerResult != kIOReturnSuccess) {
        IOLockLock(mLock);
        mEngine.markOffline();
        IOLockUnlock(mLock);
        traceStage(mOwner, "RESUME_POWER_FAILED", powerResult);
        return powerResult;
    }
#endif
    IOLockLock(mLock);
    IOReturn result = kIOReturnNotReady;
    if (mWindow.base && pciIsD0()) {
        mPCI->setMemoryEnable(true);
        if (mPCI->configRead16(kIOPCIConfigCommand) & 2U)
            result = mEngine.resume();
        else
            mEngine.markOffline();
#if defined(KERNEL)
        if (result == kIOReturnSuccess)
            result = mChipSelects.validateOutputs();
        if (result != kIOReturnSuccess)
            mEngine.markOffline();
#endif
    } else
        mEngine.markOffline();
    IOLockUnlock(mLock);
    mTracedSelects = 0;
    traceStage(mOwner, "RESUME_END", result);
    return result;
}

void IntelLPSSSPIController::shutdown() {
    traceStage(mOwner, "SHUTDOWN_BEGIN");
    // The owner must have stopped and joined all callers before this method;
    // freeing a lock while another caller waits on it is not safe.
    if (mLock)
        IOLockLock(mLock);
    if (mWindow.base && pciIsD0() && (mPCI->configRead16(kIOPCIConfigCommand) & 2U))
        mEngine.shutdown();
    else
        mEngine.abandon();
    mWindow.base = nullptr;
    // All borrowers were joined by the owner. Provider close/release may enter
    // IOKit or firmware: do not call them under our private transaction lock.
    if (mLock)
        IOLockUnlock(mLock);
#if defined(KERNEL)
    mChipSelects.shutdown();
#endif
    if (mMap) {
        mMap->release();
        mMap = nullptr;
    }
    if (mPCI) {
        if (mMemoryChanged && pciIsD0())
            mPCI->setMemoryEnable(mMemoryWasEnabled);
        mPCI->close(mOwner);
        mPCI->release();
        mPCI = nullptr;
    }
    traceStage(mOwner, "SHUTDOWN_END");
    mOwner = nullptr;
    mMemoryChanged = false;
    mTracedSelects = 0;
    if (mLock) {
        IOLockFree(mLock);
        mLock = nullptr;
    }
}

}
