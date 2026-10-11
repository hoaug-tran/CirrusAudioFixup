//
// AudioEventSource.hpp
// IOAudioFamily observations delivered to the amplifier workloop.
// See LICENSE for distribution terms.
//

#pragma once

#include <IOKit/IOEventSource.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

namespace cirrus::platform::hda {

struct AudioEventState {
    bool routeKnown{false};
    bool speakers{false};
    bool engineKnown{false};
    bool engineRunning{false};
    uint32_t generation{0};
};

}

// Hooks never acquire the amplifier workloop gate. This event source owns a
// short mailbox lock; the workloop consumes the latest state, so a rapid series
// of jack changes cannot leave a queue of obsolete power-up requests.
// OpenCore's IOKit vtable linker requires the concrete metaclass in the global
// namespace. Keep a namespace alias below for the driver-facing API.
class CirrusAudioEventSource : public IOEventSource {
    OSDeclareDefaultStructors(CirrusAudioEventSource)

public:
    using AudioEventState = cirrus::platform::hda::AudioEventState;
    using Handler = void (*)(OSObject*, const AudioEventState&);
    static CirrusAudioEventSource* create(OSObject* owner, IOService* controller, Handler handler);
    static bool hooksReady();
    bool attach();
    void refreshRoute();
    void detach();
    bool checkForWork() override;
    void free() override;
    void observe(IOService* sender, bool route, uint32_t value, bool seed = false);
    AudioEventState snapshot();
    bool unchanged(uint32_t generation);

private:
    IOService* mController{nullptr};
    IOLock* mMailboxLock{nullptr};
    Handler mHandler{nullptr};
    AudioEventState mState{};
    bool mPending{false};
    bool mAttached{false};
    // The selected engine is retained for this source's lifetime. Observations
    // from HDMI, input engines and another output engine cannot control it.
    IORegistryEntry* mEngine{nullptr};
    bool belongsToController(IORegistryEntry* entry);
};

namespace cirrus::platform::hda {
using AudioEventSource = ::CirrusAudioEventSource;
}
