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
            auto* count = OSDynamicCast(OSNumber, owner->getProperty("Cirrus_SPI_Boot_Event_Count"));
            const unsigned index = count ? count->unsigned32BitValue() : 0;
            if (index < 128) {
                char key[80]{}, value[192]{};
                uint64_t ns = 0;
                absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
                snprintf(key, sizeof(key), "Cirrus_SPI_Boot_Event_%03u", index);
                snprintf(value, sizeof(value), "uptime_us=%llu stage=%s ret=0x%08x", static_cast<unsigned long long>(ns / 1000), stage,
                         uint32_t(result));
                owner->setProperty(key, value);
                owner->setProperty("Cirrus_SPI_Boot_Event_Count", uint64_t(index + 1), 32);
            } else
                owner->setProperty("Cirrus_SPI_Boot_Event_Status", "PARTIAL_LIMIT");
            if (result != kIOReturnSuccess && !owner->getProperty("Cirrus_SPI_First_Controller_Error")) {
                owner->setProperty("Cirrus_SPI_First_Controller_Error", stage);
                owner->setProperty("Cirrus_SPI_First_Controller_Return", uint64_t(uint32_t(result)), 32);
            }
        }
        CIRRUS_LOG("BOOT_FLOW Intel_SPI owner=%llu %s ret=0x%08X", static_cast<unsigned long long>(owner ? owner->getRegistryEntryID() : 0),
                   stage, uint32_t(result));
    }
#else
    (void)owner;
    (void)stage;
    (void)result;
#endif
}

uint32_t IntelLPSSSPIController::MMIOWindow::read32(uint32_t offset) {
#if defined(KERNEL)
    if (initializing && capture)
        CIRRUS_LOG("BOOT_FLOW SPI_MMIO_READ_BEGIN offset=0x%03x", offset);
#endif
    uint32_t value = *reinterpret_cast<volatile uint32_t*>(base + offset);
    OSSynchronizeIO();
    record(offset, value, false);
#if defined(KERNEL)
    if (initializing && capture)
        CIRRUS_LOG("BOOT_FLOW SPI_MMIO_READ_END offset=0x%03x value=0x%08x", offset, value);
#endif
    return value;
}

void IntelLPSSSPIController::MMIOWindow::write32(uint32_t offset, uint32_t value) {
#if defined(KERNEL)
    if (initializing && capture)
        CIRRUS_LOG("BOOT_FLOW SPI_MMIO_WRITE_BEGIN offset=0x%03x value=0x%08x", offset, value);
#endif
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
    OSSynchronizeIO();
    record(offset, value, true);
#if defined(KERNEL)
    if (initializing && capture)
        CIRRUS_LOG("BOOT_FLOW SPI_MMIO_WRITE_END offset=0x%03x", offset);
#endif
}

void IntelLPSSSPIController::MMIOWindow::publish() const {
#if defined(KERNEL)
    if (!capture || !owner)
        return;
    const uint32_t count = accessCount < 64 ? uint32_t(accessCount) : 64;
    owner->setProperty("Cirrus_SPI_MMIO_Total", accessCount, 64);
    owner->setProperty("Cirrus_SPI_MMIO_Retained", uint64_t(count), 32);
    owner->setProperty("Cirrus_SPI_MMIO_Status", accessCount > 64 ? "LAST_64" : "COMPLETE");
    owner->setProperty("Cirrus_SPI_Engine_Last_Phase", lastPhase);
    if (firstFailedPhase) {
        owner->setProperty("Cirrus_SPI_Engine_First_Error", firstFailedPhase);
        owner->setProperty("Cirrus_SPI_Engine_First_Return", uint64_t(uint32_t(firstPhaseReturn)), 32);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t sequence = accessCount - count + i;
        const auto& entry = accesses[sequence % 64];
        char key[80]{}, value[128]{};
        snprintf(key, sizeof(key), "Cirrus_SPI_MMIO_%02u", i);
        snprintf(value, sizeof(value), "seq=%llu %s offset=0x%03x value=0x%08x", static_cast<unsigned long long>(sequence),
                 entry.write ? "WRITE" : "READ", entry.offset, entry.value);
        owner->setProperty(key, value);
    }
#endif
}

uint64_t IntelLPSSSPIController::MMIOWindow::nowUs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
void IntelLPSSSPIController::MMIOWindow::stage(const char* phase, IOReturn result) {
    lastPhase = phase;
    if (result != kIOReturnSuccess && !firstFailedPhase) {
        firstFailedPhase = phase;
        firstPhaseReturn = result;
    }
#if defined(KERNEL)
    // The engine may run under the transaction lock. Log only; do not acquire
    // registry/property locks or evaluate firmware from this callback.
    CIRRUS_LOG("BOOT_FLOW Intel_SPI owner=%llu %s ret=0x%08X", static_cast<unsigned long long>(owner ? owner->getRegistryEntryID() : 0),
               phase, uint32_t(result));
#else
    (void)phase;
    (void)result;
#endif
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

IOReturn IntelLPSSSPIController::acquirePower() {
    traceStage(mOwner, "POWER_REQUEST_BEGIN");
    if (!mPowerClient) {
        traceStage(mOwner, "POWER_CLIENT_ALLOC_BEGIN");
        mPowerClient = new IOService;
        traceStage(mOwner, "POWER_CLIENT_ALLOC_END", mPowerClient ? kIOReturnSuccess : kIOReturnNoMemory);
        if (!mPowerClient)
            return kIOReturnNoMemory;
        if (!mPowerClient->init()) {
            mPowerClient->release();
            mPowerClient = nullptr;
            return kIOReturnNoMemory;
        }
        traceStage(mOwner, "POWER_CLIENT_ATTACH_BEGIN");
        const bool attached = mPowerClient->attach(mPCI);
        traceStage(mOwner, "POWER_CLIENT_ATTACH_END", attached ? kIOReturnSuccess : kIOReturnNotReady);
        if (!attached) {
            mPowerClient->release();
            mPowerClient = nullptr;
            return kIOReturnNotReady;
        }
        traceStage(mOwner, "POWER_CLIENT_INIT_BEGIN");
        mPowerClient->PMinit();
        traceStage(mOwner, "POWER_CLIENT_INIT_END");
        mPowerStates[0].version = 1;
        mPowerStates[1].version = 1;
        mPowerStates[1].capabilityFlags = kIOPMPowerOn | kIOPMDeviceUsable;
        mPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
        traceStage(mOwner, "POWER_CLIENT_REGISTER_BEGIN");
        auto result = mPowerClient->registerPowerDriver(mPowerClient, mPowerStates, 2);
        traceStage(mOwner, "POWER_CLIENT_REGISTER_END", result);
        if (result != kIOReturnSuccess)
            return result;
        traceStage(mOwner, "POWER_CLIENT_JOIN_BEGIN");
        mPCI->joinPMtree(mPowerClient);
        traceStage(mOwner, "POWER_CLIENT_JOIN_END");
    }
    const auto result = mPowerClient->changePowerStateTo(1);
    traceStage(mOwner, "POWER_REQUEST_END", result);
    if (result != kIOReturnSuccess)
        return result;
    // The request is asynchronous. Poll configuration space only, with a
    // completed-iteration bound independent of the monotonic clock. Never
    // evaluate firmware or wait on PM while holding the SPI transaction lock.
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        const bool domainReady = (mPowerClient->currentCapability() & kIOPMDeviceUsable) != 0;
        if (domainReady && pciIsD0()) {
#if defined(KERNEL)
            if (gCirrusDebug) {
                mOwner->setProperty("Cirrus_SPI_Power_Wait_Attempts", uint64_t(attempt + 1), 32);
                mOwner->setProperty("Cirrus_SPI_Power_Client_Capability", uint64_t(mPowerClient->currentCapability()), 32);
            }
#endif
            traceStage(mOwner, "POWER_D0_READY");
            return kIOReturnSuccess;
        }
        // Firmware can leave PMCSR in D3hot while IOPCIFamily already reports
        // its logical On state (observed on the target). A PM vote alone then
        // need not cause a physical transition. Only after the child has been
        // granted power, request D0 through the standard PCI PM capability.
        // Preserve other control bits and never write 1 to W1C PME_Status.
        if (domainReady && mPCI->configRead16(0) == 0x8086 && mPCI->configRead16(2) == mDeviceId) {
            uint8_t offset = 0;
            (void)mPCI->findPCICapability(kIOPCIPowerManagementCapability, &offset);
            if (offset && offset <= 0xf8) {
                const auto pmcsrOffset = uint8_t(offset + 4);
                const uint16_t before = mPCI->configRead16(pmcsrOffset);
                if (before != 0xffff && (before & 3U)) {
                    // A D3hot exit may reset configuration unless the function
                    // advertises No_Soft_Reset. Do not reuse cached BAR resources
                    // on such hardware without a PCI-family restore contract.
                    if ((before & 3U) == 3U && !(before & 8U)) {
                        traceStage(mOwner, "PCI_D0_CONFIG_RESTORE_REQUIRED", kIOReturnNotReady);
                        return kIOReturnNotReady;
                    }
#if defined(KERNEL)
                    if (gCirrusDebug)
                        mOwner->setProperty("Cirrus_Intel_SPI_PMCSR_Before_D0", uint64_t(before), 32);
#endif
                    traceStage(mOwner, "PCI_D0_TRANSITION_BEGIN");
                    mPCI->configWrite16(pmcsrOffset, before & uint16_t(~0x8003U));
                    // D3hot -> D0 requires recovery before BAR/MMIO access.
                    IOSleep(10);
#if defined(KERNEL)
                    if (gCirrusDebug)
                        mOwner->setProperty("Cirrus_Intel_SPI_PMCSR_After_D0", uint64_t(mPCI->configRead16(pmcsrOffset)), 32);
#endif
                    traceStage(mOwner, "PCI_D0_TRANSITION_END", pciIsD0() ? kIOReturnSuccess : kIOReturnNotReady);
                    // One hardware request per acquisition, not 50 writes to
                    // a device that refuses the transition.
                    if (!pciIsD0())
                        return kIOReturnNotReady;
                }
            }
        }
        IOSleep(10);
    }
    traceStage(mOwner, "POWER_D0_TIMEOUT", kIOReturnTimeout);
    return kIOReturnTimeout;
}

void IntelLPSSSPIController::releasePower() {
    if (!mPowerClient)
        return;
    traceStage(mOwner, "POWER_RELEASE_BEGIN");
    mPowerClient->PMstop();
    mPowerClient->detach(mPCI);
    mPowerClient->release();
    mPowerClient = nullptr;
    traceStage(mOwner, "POWER_RELEASE_END");
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
        owner->setProperty("Cirrus_SPI_PCI_BAR0_Before", uint64_t(mPCI->configRead32(kIOPCIConfigBaseAddress0)), 32);
        owner->setProperty("Cirrus_SPI_PCI_BAR1_Before", uint64_t(mPCI->configRead32(kIOPCIConfigBaseAddress0 + 4)), 32);
    }
#endif
    const auto powerResult = acquirePower();
    if (powerResult != kIOReturnSuccess) {
        shutdown();
        return reject("INTEL_CONTROLLER_POWER_REQUEST_FAILED", powerResult);
    }
    mMemoryWasEnabled = (mPCI->configRead16(kIOPCIConfigCommand) & 2U) != 0;
    mPCI->setMemoryEnable(true);
    mMemoryChanged = true;
#if defined(KERNEL)
    if (gCirrusDebug) {
        owner->setProperty("Cirrus_SPI_PCI_BAR0_After", uint64_t(mPCI->configRead32(kIOPCIConfigBaseAddress0)), 32);
        owner->setProperty("Cirrus_SPI_PCI_BAR1_After", uint64_t(mPCI->configRead32(kIOPCIConfigBaseAddress0 + 4)), 32);
        owner->setProperty("Cirrus_SPI_PCI_Command_After", uint64_t(mPCI->configRead16(kIOPCIConfigCommand)), 32);
    }
#endif
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
    if (gCirrusDebug && mMap)
        owner->setProperty("Cirrus_SPI_BAR_Physical", uint64_t(mMap->getPhysicalAddress()), 64);
#endif
    if (!mMap || mMap->getLength() < LPSSSPIEngine::kMinimumWindowBytes || !mMap->getVirtualAddress()) {
        const char* reason = !mMap                                                    ? "INTEL_CONTROLLER_BAR_MAPPING_FAILED"
                             : mMap->getLength() < LPSSSPIEngine::kMinimumWindowBytes ? "INTEL_CONTROLLER_BAR_TOO_SMALL"
                                                                                      : "INTEL_CONTROLLER_BAR_ADDRESS_UNAVAILABLE";
        shutdown();
        return reject(reason, kIOReturnNoResources);
    }
    mWindow.base = reinterpret_cast<volatile uint8_t*>(mMap->getVirtualAddress());
    mWindow.owner = owner;
#if defined(KERNEL)
    mWindow.capture = gCirrusDebug;
#endif
    mWindow.accessCount = 0;
    mWindow.lastPhase = "NONE";
    mWindow.firstFailedPhase = nullptr;
    mWindow.initializing = true;
    traceStage(owner, "MMIO_INITIALIZE_BEGIN");
    IOReturn result = mEngine.initialize(mProfile);
    mWindow.initializing = false;
    mWindow.publish();
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
    if (trace || result != kIOReturnSuccess) {
        mWindow.publish();
#if defined(KERNEL)
        mChipSelects.publishEvidence();
        if (gCirrusDebug && mOwner) {
            mOwner->setProperty("Cirrus_SPI_Transfer_CS", uint64_t(endpoint.chipSelect), 32);
            mOwner->setProperty("Cirrus_SPI_Transfer_Hz", uint64_t(endpoint.clockHz), 32);
            mOwner->setProperty("Cirrus_SPI_Transfer_Mode", uint64_t(endpoint.mode), 32);
            mOwner->setProperty("Cirrus_SPI_Transfer_Bytes", uint64_t(length), 64);
            mOwner->setProperty("Cirrus_SPI_Transfer_Return", uint64_t(uint32_t(result)), 32);
            char header[65]{};
            if (tx)
                for (size_t i = 0; i < length && i < 32; ++i)
                    snprintf(header + i * 2, 3, "%02x", tx[i]);
            mOwner->setProperty("Cirrus_SPI_Transfer_TX_First32", header);
        }
#endif
    }
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
    if (mPowerClient)
        (void)mPowerClient->changePowerStateTo(0);
}

IOReturn IntelLPSSSPIController::resume() {
    if (!mLock)
        return kIOReturnNotReady;
    traceStage(mOwner, "RESUME_BEGIN");
    const auto pciPowerResult = acquirePower();
    if (pciPowerResult != kIOReturnSuccess) {
        IOLockLock(mLock);
        mEngine.markOffline();
        IOLockUnlock(mLock);
        traceStage(mOwner, "RESUME_PCI_POWER_FAILED", pciPowerResult);
        return pciPowerResult;
    }
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
    mWindow.owner = nullptr;
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
        releasePower();
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
