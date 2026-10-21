// Intel LPSS native-CS PIO transport. Hardware register behavior is described
// by the references in LPSSSPIEngine.hpp; no Linux driver implementation is
// included. Apple private SPI classes are not called or assumed ABI-compatible.
#include "IntelLPSSSPIController.hpp"
#include <kern/clock.h>
#include <mach/mach_time.h>
#if defined(KERNEL)
#include "Support/Logging.hpp"
#include "Transport/SPI/SPIPowerClient.hpp"
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

void IntelLPSSSPIController::publishPowerSnapshot(const char* phase, unsigned attempt, IOReturn result, uint64_t startedNs) {
#if defined(KERNEL)
    if (!gCirrusDebug || !mOwner || !mPCI)
        return;
    uint64_t nowNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
    char key[112]{};
    const auto number = [&](const char* field, uint64_t value) {
        snprintf(key, sizeof(key), "Cirrus_SPI_Power_%s_%s", phase, field);
        mOwner->setProperty(key, value, 64);
    };
    const auto string = [&](const char* field, const char* value) {
        snprintf(key, sizeof(key), "Cirrus_SPI_Power_%s_%s", phase, field);
        mOwner->setProperty(key, value);
    };
    mOwner->setProperty("Cirrus_SPI_Power_Status", phase);
    number("Deadline_MS", 30000);
    number("Attempt", attempt);
    number("Elapsed_US", nowNs >= startedNs ? (nowNs - startedNs) / 1000 : 0);
    number("Return", uint32_t(result));
    number("PCI_Identity", (uint32_t(mPCI->configRead16(0)) << 16) | mPCI->configRead16(2));
    number("PCI_Command", mPCI->configRead16(kIOPCIConfigCommand));
    number("BAR0", mPCI->configRead32(kIOPCIConfigBaseAddress0));
    number("BAR1", mPCI->configRead32(kIOPCIConfigBaseAddress0 + 4));
    uint8_t pmOffset = 0;
    (void)mPCI->findPCICapability(kIOPCIPowerManagementCapability, &pmOffset);
    number("PM_Offset", pmOffset);
    if (pmOffset && pmOffset <= 0xf8)
        number("PMCSR", mPCI->configRead16(uint8_t(pmOffset + 4)));
    number("Parent_Capability", mPCI->currentCapability());
    number("Client_Capability", mPowerClient ? mPowerClient->currentCapability() : 0);
    number("Parent_Power_State", mPCI->getPowerState());
    number("Client_Power_State", mPowerClient ? mPowerClient->getPowerState() : 0);
    // The PM tree connection, not the IOService attachment, establishes the
    // domain grant. Snapshot both sides before teardown removes the client.
    auto* connection = mPowerClient ? mPowerClient->getParentEntry(gIOPowerPlane) : nullptr;
    auto* powerParent = connection ? connection->getParentEntry(gIOPowerPlane) : nullptr;
    number("Client_In_Power_Plane", connection != nullptr);
    number("Power_Parent_Is_PCI", powerParent == mPCI);
    if (powerParent)
        string("Power_Parent_Name", powerParent->getName());
    char path[256]{};
    int length = sizeof(path);
    if (mPCI->getPath(path, &length, gIOServicePlane))
        string("PCI_Path", path);
    // IOPowerManagement serializes live state. Retain sampled values instead.
    string("State_Evidence", "SAMPLED_API_VALUES_NOT_LIVE_PM_OBJECT");
    if (mPowerClient) {
        const auto callbacks = static_cast<CirrusSPIPowerClient*>(mPowerClient)->snapshot();
        number("Callback_Parent_ID", callbacks.parentId);
        number("Callback_Cancelled", callbacks.cancelled);
        string("PM_Gate_Context", "UNAVAILABLE_PUBLIC_LP64_API");
        const char* names[]{"Initialized", "Registered", "Joined", "Vote", "SetState", "Completed"};
        for (unsigned i = 0; i < 6; ++i) {
            const auto& e = i < 4 ? callbacks.milestones[i] : i == 4 ? callbacks.setState : callbacks.completed;
            char field[64]{}, value[256]{};
            snprintf(field, sizeof(field), "Callback_%s", names[i]);
            snprintf(value, sizeof(value), "count=%u uptime_us=%llu thread=%llu ordinal=%llu device=%llu ret=0x%08x",
                     e.count, static_cast<unsigned long long>(e.uptimeUs), static_cast<unsigned long long>(e.threadId),
                     static_cast<unsigned long long>(e.ordinal), static_cast<unsigned long long>(e.deviceId), uint32_t(e.result));
            string(field, value);
        }
    }
#else
    (void)phase;
    (void)attempt;
    (void)result;
    (void)startedNs;
#endif
}

IOReturn IntelLPSSSPIController::acquirePower() {
    uint64_t nowNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
    const bool newRequest = !mPowerPending;
    if (newRequest) {
        mPowerStartedNs = nowNs;
        mPowerSamples = 0;
        publishPowerSnapshot("Before", 0, kIOReturnSuccess, mPowerStartedNs);
        traceStage(mOwner, "POWER_REQUEST_BEGIN");
    }
    const uint64_t startedNs = mPowerStartedNs;
    // Abort a pending request if the PCI function disappears or changes.
    if (mPCI->configRead16(0) != 0x8086 || mPCI->configRead16(2) != mDeviceId) {
        mPowerPending = false;
        traceStage(mOwner, "PCI_IDENTITY_UNAVAILABLE_OR_CHANGED", kIOReturnNoDevice);
        publishPowerSnapshot("Rejected", mPowerSamples, kIOReturnNoDevice, startedNs);
        return kIOReturnNoDevice;
    }
    if (!mPowerClient) {
        traceStage(mOwner, "POWER_CLIENT_ALLOC_BEGIN");
#if defined(KERNEL)
        mPowerClient = new CirrusSPIPowerClient;
#else
        mPowerClient = new IOService;
#endif
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
#if defined(KERNEL)
        static_cast<CirrusSPIPowerClient*>(mPowerClient)->identifyParent(mPCI);
        static_cast<CirrusSPIPowerClient*>(mPowerClient)->recordMilestone(CirrusSPIPowerClient::Milestone::Initialized);
#endif
        traceStage(mOwner, "POWER_CLIENT_INIT_END");
        mPowerStates[0].version = 1;
        mPowerStates[1].version = 1;
        mPowerStates[1].capabilityFlags = kIOPMPowerOn | kIOPMDeviceUsable;
        mPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
        traceStage(mOwner, "POWER_CLIENT_REGISTER_BEGIN");
        auto result = mPowerClient->registerPowerDriver(mPowerClient, mPowerStates, 2);
#if defined(KERNEL)
        static_cast<CirrusSPIPowerClient*>(mPowerClient)->recordMilestone(CirrusSPIPowerClient::Milestone::Registered, result);
#endif
        traceStage(mOwner, "POWER_CLIENT_REGISTER_END", result);
        if (result != kIOReturnSuccess)
            return result;
        traceStage(mOwner, "POWER_CLIENT_JOIN_BEGIN");
        mPCI->joinPMtree(mPowerClient);
#if defined(KERNEL)
        static_cast<CirrusSPIPowerClient*>(mPowerClient)->recordMilestone(CirrusSPIPowerClient::Milestone::Joined);
#endif
        traceStage(mOwner, "POWER_CLIENT_JOIN_END");
    }
    if (newRequest) {
        const auto result = mPowerClient->changePowerStateTo(1);
#if defined(KERNEL)
        static_cast<CirrusSPIPowerClient*>(mPowerClient)->recordMilestone(CirrusSPIPowerClient::Milestone::Vote, result, 1);
#endif
        traceStage(mOwner, "POWER_REQUEST_END", result);
        publishPowerSnapshot("Requested", 0, result, startedNs);
        if (result != kIOReturnSuccess)
            return result;
        mPowerPending = true;
    }
    // The owner polls from its workloop while the PM request remains queued.
    const unsigned attempt = mPowerSamples++;
    {
        const auto capability = mPowerClient->currentCapability();
        const bool domainReady = (capability & kIOPMDeviceUsable) != 0;
#if defined(KERNEL)
        // Preserve the last power sample before failure cleanup removes the client.
        if (gCirrusDebug && mOwner && (attempt == 0 || domainReady)) {
            mOwner->setProperty("Cirrus_SPI_Power_Wait_Attempts", uint64_t(attempt + 1), 32);
            mOwner->setProperty("Cirrus_SPI_Power_Client_Capability", uint64_t(capability), 32);
            mOwner->setProperty("Cirrus_SPI_Power_Parent_Capability", uint64_t(mPCI->currentCapability()), 32);
            uint8_t pmOffset = 0;
            (void)mPCI->findPCICapability(kIOPCIPowerManagementCapability, &pmOffset);
            if (pmOffset && pmOffset <= 0xf8)
                mOwner->setProperty("Cirrus_SPI_Power_Wait_PMCSR", uint64_t(mPCI->configRead16(uint8_t(pmOffset + 4))), 32);
            mOwner->setProperty("Cirrus_SPI_Power_Wait_Reason", domainReady ? "WAITING_PCI_D0" : "WAITING_DOMAIN_GRANT");
        }
#endif
        if (domainReady && pciIsD0()) {
#if defined(KERNEL)
            if (gCirrusDebug && mOwner)
                mOwner->setProperty("Cirrus_SPI_Power_Wait_Reason", "READY");
#endif
            traceStage(mOwner, "POWER_D0_READY");
            publishPowerSnapshot("Ready", attempt + 1, kIOReturnSuccess, startedNs);
            mPowerPending = false;
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
                        publishPowerSnapshot("Rejected", attempt + 1, kIOReturnNotReady, startedNs);
                        traceStage(mOwner, "PCI_D0_CONFIG_RESTORE_REQUIRED", kIOReturnNotReady);
                        mPowerPending = false;
                        return kIOReturnNotReady;
                    }
#if defined(KERNEL)
                    if (gCirrusDebug)
                        mOwner->setProperty("Cirrus_Intel_SPI_PMCSR_Before_D0", uint64_t(before), 32);
#endif
                    traceStage(mOwner, "PCI_D0_TRANSITION_BEGIN");
                    publishPowerSnapshot("Transition_Before", attempt + 1, kIOReturnSuccess, startedNs);
                    mPCI->configWrite16(pmcsrOffset, before & uint16_t(~0x8003U));
                    // D3hot -> D0 requires recovery before BAR/MMIO access.
                    IOSleep(10);
#if defined(KERNEL)
                    if (gCirrusDebug)
                        mOwner->setProperty("Cirrus_Intel_SPI_PMCSR_After_D0", uint64_t(mPCI->configRead16(pmcsrOffset)), 32);
#endif
                    traceStage(mOwner, "PCI_D0_TRANSITION_END", pciIsD0() ? kIOReturnSuccess : kIOReturnNotReady);
                    publishPowerSnapshot("Transition_After", attempt + 1, pciIsD0() ? kIOReturnSuccess : kIOReturnNotReady, startedNs);
                    // Request D0 once per acquisition.
                    mPowerPending = false;
                    if (!pciIsD0())
                        return kIOReturnNotReady;
                    publishPowerSnapshot("Ready", attempt + 1, kIOReturnSuccess, startedNs);
                    traceStage(mOwner, "POWER_D0_READY");
                    return kIOReturnSuccess;
                }
            }
        }
    }
    if (nowNs - startedNs < 30000000000ULL) {
        if (attempt == 0)
            traceStage(mOwner, "POWER_DOMAIN_PENDING");
        if (attempt % 100 == 0)
            publishPowerSnapshot("Pending", attempt + 1, kIOReturnNotReady, startedNs);
        return kIOReturnNotReady;
    }
    const bool grantMissing = (mPowerClient->currentCapability() & kIOPMDeviceUsable) == 0;
#if defined(KERNEL)
    if (gCirrusDebug && mOwner) {
        const auto callbacks = static_cast<CirrusSPIPowerClient*>(mPowerClient)->snapshot();
        const char* blocker = "PCI_DSTATE_NOT_D0";
        if (mPCI->configRead16(0) != 0x8086 || mPCI->configRead16(2) != mDeviceId)
            blocker = "PCI_IDENTITY_UNAVAILABLE_OR_CHANGED";
        else if (grantMissing) {
            if (!mPowerClient->getParentEntry(gIOPowerPlane))
                blocker = "PM_CLIENT_NOT_JOINED_BEFORE_DEADLINE";
            else if (!callbacks.setState.count)
                blocker = "PM_CLIENT_CALLBACK_NOT_DELIVERED";
            else if (!callbacks.completed.count)
                blocker = "PM_CLIENT_TRANSITION_NOT_COMPLETED";
            else
                blocker = "PM_CLIENT_STATE_NOT_USABLE";
        }
        mOwner->setProperty("Cirrus_SPI_Power_Blocker", blocker);
    }
#endif
    traceStage(mOwner, grantMissing ? "POWER_DOMAIN_GRANT_TIMEOUT" : "POWER_PCI_D0_TIMEOUT", kIOReturnTimeout);
    publishPowerSnapshot("Timeout", attempt + 1, kIOReturnTimeout, startedNs);
    mPowerPending = false;
    return kIOReturnTimeout;
}

void IntelLPSSSPIController::releasePower() {
    if (!mPowerClient)
        return;
    traceStage(mOwner, "POWER_RELEASE_BEGIN");
#if defined(KERNEL)
    static_cast<CirrusSPIPowerClient*>(mPowerClient)->cancelAndStop(true);
#else
    mPowerClient->PMstop();
#endif
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
    return continueBind();
}

IOReturn IntelLPSSSPIController::continueBind() {
    if (!mPCI || !mLock || !mOwner)
        return kIOReturnNotReady;
    if (mWindow.base)
        return kIOReturnSuccess;
    auto* owner = mOwner;
    auto reject = [this](const char* reason, IOReturn result) {
        mBindFailureReason = reason;
        traceStage(mOwner, reason, result);
        return result;
    };
    const auto powerResult = acquirePower();
    if (powerResult != kIOReturnSuccess) {
        if (mPowerPending)
            return powerResult;
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
#if defined(KERNEL)
    owner->setProperty("Cirrus_SPI_Clock_Initial", uint64_t(mEngine.initialClock()), 32);
    owner->setProperty("Cirrus_SPI_Clock_Prepared", uint64_t(mEngine.preparedClock()), 32);
    owner->setProperty("Cirrus_SPI_Clock_Gate_Initial", uint64_t(mEngine.initialClockGate()), 32);
    owner->setProperty("Cirrus_SPI_Clock_Rate_Source", "PROGRAMMED_DIVIDER_NOT_MEASURED");
#endif
    mWindow.initializing = false;
    mWindow.publish();
    traceStage(owner, "MMIO_INITIALIZE_END", result);
    if (result != kIOReturnSuccess) {
        shutdown();
        return reject("INTEL_CONTROLLER_ENGINE_INITIALIZATION_FAILED", result);
    }
    return result;
}

#if defined(KERNEL)
IOReturn IntelLPSSSPIController::writeGPIOGroup(bool level) {
    if (!mLock)
        return kIOReturnNotReady;
    const auto power = mChipSelects.validatePower();
    if (power != kIOReturnSuccess)
        return power;
    traceStage(mOwner, level ? "GPIO_GROUP_HIGH_BEGIN" : "GPIO_GROUP_LOW_BEGIN");
    IOLockLock(mLock);
    const auto result = mWindow.base && pciIsD0() ? mChipSelects.writeGPIOGroup(level) : kIOReturnNotReady;
    IOLockUnlock(mLock);
    mChipSelects.publishGPIOGroupEvidence();
    traceStage(mOwner, level ? "GPIO_GROUP_HIGH_END" : "GPIO_GROUP_LOW_END", result);
    if (mOwner) {
        mOwner->setProperty("Cirrus_SPI_GPIO_Group_Level", uint64_t(level), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Group_Return", uint64_t(uint32_t(result)), 32);
    }
    return result;
}
#endif

void IntelLPSSSPIController::publishTransactionEvidence(const char* prefix, const LPSSSPIEngine::TransactionEvidence& e) {
#if defined(KERNEL)
    if (!gCirrusDebug || !mOwner || !e.valid)
        return;
    char key[128]{}, value[512]{};
    const auto publish = [&](const char* field, const char* data) {
        snprintf(key, sizeof(key), "%s_%s", prefix, field);
        mOwner->setProperty(key, data);
    };
    publish("Phase", e.phase);
    snprintf(value, sizeof(value), "seq=%llu cs=%u external=%u mode=%u requested_hz=%u calculated_hz=%u bytes=%llu sent=%llu received=%llu configured=%u ret=0x%08x cleanup=0x%08x",
             static_cast<unsigned long long>(e.sequence), e.chipSelect, unsigned(e.externalCS), e.mode, e.requestedHz, e.actualHz,
             static_cast<unsigned long long>(e.length), static_cast<unsigned long long>(e.sent), static_cast<unsigned long long>(e.received),
             unsigned(e.configured), uint32_t(e.result), uint32_t(e.cleanupResult));
    publish("Transfer", value);
    snprintf(value, sizeof(value), "attempted=%u primary=0x%08x controller=0x%08x native_cs=0x%08x external_cs=0x%08x drain=0x%08x cleanup=0x%08x",
             unsigned(e.cleanupAttempted), uint32_t(e.primaryResult), uint32_t(e.cleanupController), uint32_t(e.cleanupNativeCS),
             uint32_t(e.cleanupExternalCS), uint32_t(e.cleanupDrain), uint32_t(e.cleanupResult));
    publish("Cleanup", value);
    snprintf(value, sizeof(value), "clock=0x%08x reset=0x%08x gate=0x%08x status=0x%08x cr0=0x%08x cr1=0x%08x timeout=0x%08x protocol=0x%08x cs_inactive=0x%08x cs_active=0x%08x cs_final=0x%08x",
             e.clock, e.reset, e.gate, e.status, e.control0, e.control1, e.timeout, e.protocol, e.csInactive, e.csActive, e.csFinal);
    publish("Registers", value);
    char bytes[65]{};
    for (size_t i = 0; i < e.length && i < sizeof(e.tx); ++i)
        snprintf(bytes + i * 2, 3, "%02x", e.tx[i]);
    publish("TX", bytes);
    bytes[0] = 0;
    for (size_t i = 0; i < e.received && i < sizeof(e.rx); ++i)
        snprintf(bytes + i * 2, 3, "%02x", e.rx[i]);
    publish("RX", bytes);
#else
    (void)prefix;
    (void)e;
#endif
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
#if defined(KERNEL)
    LPSSSPIEngine::TransactionEvidence transaction{}, firstFailure{}, firstCS{};
    bool engineAttempted = false;
#endif
    if (mWindow.base && pciIsD0() && (mPCI->configRead16(kIOPCIConfigCommand) & 2U)) {
#if defined(KERNEL)
        result = mChipSelects.validateOutputs();
        if (result == kIOReturnSuccess) {
            engineAttempted = true;
            result = mEngine.exchange(endpoint, tx, rx, length, mChipSelects.select(endpoint.chipSelect));
        }
#else
        result = mEngine.exchange(endpoint, tx, rx, length);
#endif
    }
#if defined(KERNEL)
    // Copy while serialized; publish properties only after dropping the SPI
    // lock. No extra FIFO reads or bus transactions are made for diagnostics.
    if (gCirrusDebug && engineAttempted) {
        transaction = mEngine.lastTransaction();
        firstFailure = mEngine.firstFailure();
        if (const auto* first = mEngine.firstTransaction(endpoint.chipSelect))
            firstCS = *first;
    }
#endif
    IOLockUnlock(mLock);
#if defined(KERNEL)
    if (gCirrusDebug && mOwner) {
        if (trace || result != kIOReturnSuccess) {
            mOwner->setProperty("Cirrus_SPI_Transaction_Invoked", engineAttempted ? kOSBooleanTrue : kOSBooleanFalse);
            if (!engineAttempted) {
                const char* fields[]{"Phase", "Transfer", "Cleanup", "Registers", "TX", "RX"};
                for (const char* field : fields) {
                    char key[112]{};
                    snprintf(key, sizeof(key), "Cirrus_SPI_Transaction_Last_%s", field);
                    mOwner->removeProperty(key);
                }
            }
            publishTransactionEvidence("Cirrus_SPI_Transaction_Last", transaction);
            publishTransactionEvidence("Cirrus_SPI_Transaction_FirstFailure", firstFailure);
            char prefix[96]{};
            snprintf(prefix, sizeof(prefix), "Cirrus_SPI_CS_%u_FirstFrame", endpoint.chipSelect);
            publishTransactionEvidence(prefix, firstCS);
        }
        // Keep one exact identity frame per endpoint/rate, even if a successful
        // all-zero response is not a transport error. Repeats cannot erase it.
        if (transaction.valid && tx && length == 10 && tx[0] == 0x80 && !tx[1] && !tx[2] &&
            (tx[3] == 0 || tx[3] == 4) && !tx[4] && !tx[5]) {
            char prefix[96]{}, key[112]{};
            snprintf(prefix, sizeof(prefix), "Cirrus_SPI_CS_%u_Hz_%u_ID_%u", endpoint.chipSelect, endpoint.clockHz, unsigned(tx[3]));
            snprintf(key, sizeof(key), "%s_Phase", prefix);
            if (!mOwner->getProperty(key))
                publishTransactionEvidence(prefix, transaction);
        }
    }
#endif
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
            char incoming[65]{}, key[80]{};
            if (rx && result == kIOReturnSuccess)
                for (size_t i = 0; i < length && i < 32; ++i)
                    snprintf(incoming + i * 2, 3, "%02x", rx[i]);
            mOwner->setProperty("Cirrus_SPI_Transfer_RX_First32", incoming);
            mOwner->setProperty("Cirrus_SPI_Transfer_RX_Status", !rx                          ? "NOT_REQUESTED"
                                                                 : result == kIOReturnSuccess ? "COMPLETE"
                                                                                              : "TRANSFER_FAILED");
            mOwner->setProperty("Cirrus_SPI_Transfer_Actual_Hz", uint64_t(mEngine.actualClockHz()), 32);
            // Keep the first frame for each endpoint instead of overwriting
            // CS0 evidence when CS1 is probed. Failed buffers are never read.
            if (trace) {
                snprintf(key, sizeof(key), "Cirrus_SPI_CS_%u_First_TX", endpoint.chipSelect);
                mOwner->setProperty(key, header);
                snprintf(key, sizeof(key), "Cirrus_SPI_CS_%u_First_RX", endpoint.chipSelect);
                mOwner->setProperty(key, incoming);
                snprintf(key, sizeof(key), "Cirrus_SPI_CS_%u_First_Return", endpoint.chipSelect);
                mOwner->setProperty(key, uint64_t(uint32_t(result)), 32);
            }
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
    mPowerPending = false;
    if (mPowerClient)
        (void)mPowerClient->changePowerStateTo(0);
}

IOReturn IntelLPSSSPIController::resume() {
    if (!mLock)
        return kIOReturnNotReady;
    if (!mPowerPending)
        traceStage(mOwner, "RESUME_BEGIN");
    const auto pciPowerResult = acquirePower();
    if (pciPowerResult != kIOReturnSuccess) {
        IOLockLock(mLock);
        mEngine.markOffline();
        IOLockUnlock(mLock);
        if (!mPowerPending)
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
    mPowerPending = false;
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
