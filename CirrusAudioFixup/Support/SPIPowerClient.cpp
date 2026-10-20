#include "SPIPowerClient.hpp"

#if defined(KERNEL)
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>

OSDefineMetaClassAndStructors(CirrusSPIPowerClient, IOService)

bool CirrusSPIPowerClient::init(OSDictionary* properties) {
    if (!IOService::init(properties))
        return false;
    mEvidenceLock = IOLockAlloc();
    return mEvidenceLock != nullptr;
}

void CirrusSPIPowerClient::free() {
    // The owner must cancelAndStop before dropping its allocation reference.
    if (mEvidenceLock) {
        IOLockFree(mEvidenceLock);
        mEvidenceLock = nullptr;
    }
    IOService::free();
}

CirrusSPIPowerClient::Event CirrusSPIPowerClient::event(unsigned long ordinal,
                                                     uint64_t deviceId, IOReturn result) {
    Event value {};
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    value.uptimeUs = ns / 1000;
    value.threadId = thread_tid(current_thread());
    value.ordinal = ordinal;
    value.deviceId = deviceId;
    value.result = result;
    return value;
}

void CirrusSPIPowerClient::update(Event& destination, const Event& value) {
    const uint32_t count = destination.count;
    destination = value;
    destination.count = count == UINT32_MAX ? count : count + 1;
}

void CirrusSPIPowerClient::identifyParent(IOService* parent) {
    const auto id = parent ? parent->getRegistryEntryID() : 0;
    IOLockLock(mEvidenceLock);
    if (!mSnapshot.cancelled)
        mSnapshot.parentId = id;
    IOLockUnlock(mEvidenceLock);
}

void CirrusSPIPowerClient::recordMilestone(Milestone phase, IOReturn result,
                                          unsigned long ordinal) {
    const unsigned index = static_cast<unsigned>(phase);
    if (index >= static_cast<unsigned>(Milestone::Count))
        return;
    const auto value = event(ordinal, 0, result);
    IOLockLock(mEvidenceLock);
    if (!mSnapshot.cancelled)
        update(mSnapshot.milestones[index], value);
    IOLockUnlock(mEvidenceLock);
}

CirrusSPIPowerClient::Snapshot CirrusSPIPowerClient::snapshot() {
    IOLockLock(mEvidenceLock);
    const auto value = mSnapshot;
    IOLockUnlock(mEvidenceLock);
    return value;
}

void CirrusSPIPowerClient::cancelAndStop(bool pmInitialized) {
    const auto value = event(0, 0, kIOReturnSuccess);
    IOLockLock(mEvidenceLock);
    const bool alreadyCancelled = mSnapshot.cancelled;
    mSnapshot.cancelled = true;
    if (!alreadyCancelled)
        mSnapshot.cancelledUs = value.uptimeUs;
    IOLockUnlock(mEvidenceLock);
    // Never hold the evidence lock across the callback drain.
    if (!alreadyCancelled && pmInitialized)
        PMstop();
}

IOReturn CirrusSPIPowerClient::setPowerState(unsigned long ordinal, IOService* device) {
    const auto value = event(ordinal, device ? device->getRegistryEntryID() : 0,
                             kIOReturnSuccess);
    IOLockLock(mEvidenceLock);
    if (!mSnapshot.cancelled)
        update(mSnapshot.setState, value);
    IOLockUnlock(mEvidenceLock);
    return IOPMAckImplied;
}

void CirrusSPIPowerClient::powerChangeDone(unsigned long previousState) {
    const auto value = event(previousState, getRegistryEntryID(), kIOReturnSuccess);
    IOLockLock(mEvidenceLock);
    if (!mSnapshot.cancelled)
        update(mSnapshot.completed, value);
    IOLockUnlock(mEvidenceLock);
    IOService::powerChangeDone(previousState);
}
#endif
