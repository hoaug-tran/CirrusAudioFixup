#pragma once

#if defined(KERNEL)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

// Records PM callbacks. The owner stops PM before detaching and releasing.
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
        // Cached before registration to avoid registry access in callbacks.
        uint64_t parentId {0};
        uint64_t cancelledUs {0};
        Event milestones[static_cast<unsigned>(Milestone::Count)] {};
        Event setState {};
        Event completed {};
        bool cancelled {false};
    };

    bool init(OSDictionary* properties = nullptr) override;
    void free() override;

    // Cache the parent ID before registering PM.
    void identifyParent(IOService* parent);
    // Submission results are separate from callback completion.
    void recordMilestone(Milestone phase, IOReturn result = kIOReturnSuccess,
                         unsigned long ordinal = 0);
    Snapshot snapshot();
    // Call without owner locks held; PMstop waits for outstanding callbacks.
    void cancelAndStop(bool pmInitialized);

    IOReturn setPowerState(unsigned long ordinal, IOService* device) override;

protected:
    // The argument is the previous power-state ordinal.
    void powerChangeDone(unsigned long previousState) override;

private:
    IOLock* mEvidenceLock {nullptr};
    Snapshot mSnapshot {};
    static Event event(unsigned long ordinal, uint64_t deviceId, IOReturn result);
    static void update(Event& destination, const Event& value);
};
#endif
