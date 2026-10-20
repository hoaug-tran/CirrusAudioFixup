#pragma once

#if defined(KERNEL)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

// An observable PM vote holder, not an asynchronous transport scheduler.
// Owner serializes setup/milestones/cancellation and keeps its allocation
// reference until cancelAndStop(), detach(), and release() have completed.
// Callbacks retain no owner, dispatch no work, and touch no PCI/MMIO/AML.
class CirrusSPIPowerClient : public IOService {
    OSDeclareDefaultStructors(CirrusSPIPowerClient)

public:
    enum class Milestone : unsigned {
        Initialized, Registered, Joined, Vote, Count
    };
    struct Event {
        uint64_t uptimeUs {0};
        uint64_t threadId {0};
        uint64_t ordinal {0};
        uint64_t deviceId {0};
        uint32_t count {0};
        IOReturn result {kIOReturnSuccess};
    };
    struct Snapshot {
        uint64_t parentId {0}; // Cached during owner setup, not queried in callbacks.
        uint64_t cancelledUs {0};
        Event milestones[static_cast<unsigned>(Milestone::Count)] {};
        Event setState {};
        Event completed {};
        bool cancelled {false};
        bool pmGateContextAvailable {false}; // getPMworkloop unavailable on LP64.
    };

    bool init(OSDictionary* properties = nullptr) override;
    void free() override;

    // Call after attach, before PMinit/register/join/vote. No parent retain:
    // only its registry ID is copied. No owner pointer is stored anywhere.
    void identifyParent(IOService* parent);
    // Record RETURN from each owner-issued operation. Successful register/join/
    // vote milestones mean submission/return, never completed power delivery.
    void recordMilestone(Milestone phase, IOReturn result = kIOReturnSuccess,
                         unsigned long ordinal = 0);
    Snapshot snapshot();
    // Owner-only; call outside callback/owner locks, before owner destruction.
    // pmInitialized must reflect whether the owner actually called PMinit().
    // PMstop drains PM callbacks; owner still owns detach/release afterwards.
    void cancelAndStop(bool pmInitialized);

    IOReturn setPowerState(unsigned long ordinal, IOService* device) override;

protected:
    // Documented completion hook; argument is the PREVIOUS state ordinal.
    // No powerStateDidChangeTo override or interested-driver registration.
    void powerChangeDone(unsigned long previousState) override;

private:
    IOLock* mEvidenceLock {nullptr};
    Snapshot mSnapshot {};
    static Event event(unsigned long ordinal, uint64_t deviceId, IOReturn result);
    static void update(Event& destination, const Event& value);
};
#endif
