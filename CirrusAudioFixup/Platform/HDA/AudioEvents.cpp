//
// AudioEvents.cpp
// Preserve IOAudioFamily behavior and observe completed state changes.
// Hooks remain installed until shutdown; module unload is rejected.
// See LICENSE for distribution terms.
//

#define PRODUCT_NAME CirrusAudioFixup
#include <Headers/kern_api.hpp>
#include "AudioEvents.hpp"
#include <libkern/c++/OSNumber.h>
#include <IOKit/IORegistryEntry.h>

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

using cirrus::platform::hda::AudioEventSource;
using cirrus::platform::hda::AudioEventState;

// Match IOResources so module-start registration does not wait for the I2C
// provider. Lilu closes its registration window before late device matching;
// this service only anchors early loading and never touches the amplifier.
class CirrusAudioBootstrap : public IOService {
    OSDeclareDefaultStructors(CirrusAudioBootstrap)
public:
    bool start(IOService* provider) override {
        if (checkKernelArgument("-cirrusoff"))
            return false;
        return IOService::start(provider);
    }
};

OSDefineMetaClassAndStructors(CirrusAudioBootstrap, IOService)

namespace {
constexpr uint32_t kSelectorType = 0x736C6374;
constexpr uint32_t kOutputControl = 0x6F757470;
constexpr uint32_t kInternalSpeaker = 0x6973706B;
constexpr uint32_t kHeadphones = 0x6864706E;
constexpr uint32_t kEngineRunning = 1;
IOLock* listenersLock = nullptr;
AudioEventSource* listeners[4]{};
bool controlHook = false;
bool engineHook = false;
bool registered = false;
mach_vm_address_t originalUpdateValue = 0;
mach_vm_address_t originalSetState = 0;
const char* audioPaths[] = {"/System/Library/Extensions/IOAudioFamily.kext/Contents/MacOS/IOAudioFamily"};
KernelPatcher::KextInfo audioInfo[] = {{"com.apple.iokit.IOAudioFamily", audioPaths, 1, {true}, {}, 0}};

bool readNumber(IORegistryEntry* entry, const char* key, uint32_t& value) {
    // Registry values can be replaced on another thread. A retained property
    // keeps the old OSNumber alive until this observation has finished.
    auto* property = entry->copyProperty(key);
    auto* number = OSDynamicCast(OSNumber, property);
    if (number)
        value = number->unsigned32BitValue();
    if (property)
        property->release();
    return number != nullptr;
}

// Retain listeners while holding the list lock, then drop it before registry
// inspection or notification. Teardown removes a listener before releasing its
// event source; an in-flight observer therefore always owns a live reference.
void publish(IOService* sender, bool route, uint32_t value) {
    AudioEventSource* targets[4]{};
    if (!listenersLock)
        return;
    IOLockLock(listenersLock);
    for (size_t i = 0; i < 4; ++i) {
        targets[i] = listeners[i];
        if (targets[i])
            targets[i]->retain();
    }
    IOLockUnlock(listenersLock);
    for (auto* target : targets) {
        if (target) {
            target->observe(sender, route, value);
            target->release();
        }
    }
}

IOReturn updateValue(IOService* control, OSObject* value) {
    auto result = reinterpret_cast<IOReturn (*)(IOService*, OSObject*)>(originalUpdateValue)(control, value);
    if (result != kIOReturnSuccess)
        return result;
    uint32_t type = 0, subtype = 0, usage = 0;
    auto* number = OSDynamicCast(OSNumber, value);
    // FourCC values are IOAudioTypes.h ABI constants. Only an output selector
    // can change the speaker route; volume and microphone controls are ignored.
    if (number && readNumber(control, "IOAudioControlType", type) && type == kSelectorType &&
        readNumber(control, "IOAudioControlSubType", subtype) && subtype == kOutputControl &&
        readNumber(control, "IOAudioControlUsage", usage) && usage == kOutputControl)
        publish(control, true, number->unsigned32BitValue());
    return result;
}

uint32_t setState(IOService* engine, uint32_t state) {
    auto previous = reinterpret_cast<uint32_t (*)(IOService*, uint32_t)>(originalSetState)(engine, state);
    publish(engine, false, state);
    return previous;
}

void loaded(void*, KernelPatcher& patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (index != audioInfo[0].loadIndex)
        return;
    // These signatures come from IOAudioFamily source and the kernel SDK,
    // rather than private AppleHDA class layouts or guessed vtable offsets.
    KernelPatcher::RouteRequest controlRequest("__ZN14IOAudioControl11updateValueEP8OSObject", updateValue, originalUpdateValue);
    if (!__atomic_load_n(&controlHook, __ATOMIC_ACQUIRE))
        __atomic_store_n(&controlHook, patcher.routeMultiple(index, &controlRequest, 1, address, size), __ATOMIC_RELEASE);
    patcher.clearError();
    KernelPatcher::RouteRequest engineRequest("__ZN13IOAudioEngine8setStateE19_IOAudioEngineState", setState, originalSetState);
    if (!__atomic_load_n(&engineHook, __ATOMIC_ACQUIRE))
        __atomic_store_n(&engineHook, patcher.routeMultiple(index, &engineRequest, 1, address, size), __ATOMIC_RELEASE);
    patcher.clearError();
    IOLog("CirrusAudioFixup: IOAudioFamily event hooks: output=%d engine=%d\n", controlHook, engineHook);
}
}

extern "C" __attribute__((visibility("default"))) kern_return_t CirrusAudioFixup_start(kmod_info_t*, void*) {
    if (checkKernelArgument("-cirrusoff"))
        return KERN_SUCCESS;
    listenersLock = IOLockAlloc();
    if (!listenersLock)
        return KERN_FAILURE;
    auto error = lilu.requestAccess();
    if (error == LiluAPI::Error::NoError) {
        registered = lilu.onKextLoad(audioInfo, 1, loaded) == LiluAPI::Error::NoError;
        lilu.releaseAccess();
    }
    if (!registered)
        IOLog("CirrusAudioFixup: event registration unavailable; HDA observation fallback remains active\n");
    return KERN_SUCCESS;
}

extern "C" __attribute__((visibility("default"))) kern_return_t CirrusAudioFixup_stop(kmod_info_t*, void*) {
    // A callback or routed function may still refer to this image, even when
    // its provider stopped. Lilu plugins cannot safely unload after registration.
    if (registered)
        return KERN_FAILURE;
    if (listenersLock) {
        IOLockFree(listenersLock);
        listenersLock = nullptr;
    }
    return KERN_SUCCESS;
}

namespace cirrus::platform::hda {
OSDefineMetaClassAndStructors(AudioEventSource, IOEventSource)

bool AudioEventSource::hooksReady() {
    return __atomic_load_n(&controlHook, __ATOMIC_ACQUIRE) && __atomic_load_n(&engineHook, __ATOMIC_ACQUIRE);
}

AudioEventSource* AudioEventSource::create(OSObject* target, IOService* controller, Handler handler) {
    if (!target || !controller || !handler)
        return nullptr;
    auto* source = new AudioEventSource;
    if (!source)
        return nullptr;
    if (!source->init(target) || !(source->mMailboxLock = IOLockAlloc())) {
        source->release();
        return nullptr;
    }
    source->mController = controller;
    controller->retain();
    source->mHandler = handler;
    return source;
}

bool AudioEventSource::belongsToController(IORegistryEntry* entry) {
    entry->retain();
    bool found = false;
    for (size_t depth = 0; entry && depth < 64; ++depth) {
        if (entry == mController) {
            found = true;
            break;
        }
        auto* parent = entry->copyParentEntry(gIOServicePlane);
        entry->release();
        entry = parent;
    }
    if (entry)
        entry->release();
    return found;
}

bool AudioEventSource::attach() {
    if (!hooksReady() || !listenersLock)
        return false;
    IOLockLock(listenersLock);
    IOLockLock(mMailboxLock);
    if (mAttached) {
        IOLockUnlock(mMailboxLock);
        IOLockUnlock(listenersLock);
        return true;
    }
    for (auto& listener : listeners) {
        if (!listener) {
            listener = this;
            mAttached = true;
            break;
        }
    }
    const bool attached = mAttached;
    IOLockUnlock(mMailboxLock);
    IOLockUnlock(listenersLock);
    if (!attached)
        return false;
    // Seed from current registry state: the selector may have changed before
    // the amplifier service attached, with no later event until the next jack
    // operation. Use the same validation as live observations.
    auto* iterator = IORegistryIterator::iterateOver(mController, gIOServicePlane, kIORegistryIterateRecursively);
    if (iterator) {
        while (auto* entry = iterator->getNextObject()) {
            auto* service = OSDynamicCast(IOService, entry);
            if (!service)
                continue;
            uint32_t type = 0, subtype = 0, usage = 0, value = 0;
            if (readNumber(service, "IOAudioControlType", type) && type == kSelectorType &&
                readNumber(service, "IOAudioControlSubType", subtype) && subtype == kOutputControl &&
                readNumber(service, "IOAudioControlUsage", usage) && usage == kOutputControl &&
                readNumber(service, "IOAudioControlValue", value))
                observe(service, true, value, true);
        }
        iterator->release();
    }
    return true;
}

void AudioEventSource::detach() {
    if (!listenersLock)
        return;
    IOLockLock(listenersLock);
    for (auto& listener : listeners)
        if (listener == this)
            listener = nullptr;
    IOLockUnlock(listenersLock);
    IOLockLock(mMailboxLock);
    mAttached = false;
    mPending = false;
    IOLockUnlock(mMailboxLock);
}

void AudioEventSource::observe(IOService* sender, bool route, uint32_t value, bool seed) {
    if (!belongsToController(sender))
        return;
    IORegistryEntry* engine = nullptr;
    if (route) {
        engine = sender->copyParentEntry(gIOServicePlane);
        // A selector is useful only when it belongs to an audio engine. This
        // prevents a controller-scoped control from binding an unrelated engine.
        if (!engine || !engine->metaCast("IOAudioEngine")) {
            if (engine)
                engine->release();
            return;
        }
    }
    IOLockLock(mMailboxLock);
    // Subscription is already live during registry enumeration. A callback
    // can therefore publish a newer route after the iterator reads an old
    // value. Initial seeding must never overwrite a completed observation.
    if (!mAttached || (seed && mState.routeKnown) || (mEngine && (route ? engine : sender) != mEngine)) {
        IOLockUnlock(mMailboxLock);
        if (engine)
            engine->release();
        return;
    }
    if (route && !mEngine) {
        // Only the built-in speaker/headphone engine identifies this binding.
        // Unknown selectors and digital engines must not capture it first.
        if (value != kInternalSpeaker && value != kHeadphones) {
            IOLockUnlock(mMailboxLock);
            engine->release();
            return;
        }
        mEngine = engine;
        engine = nullptr;
    }
    if (!mEngine) {
        IOLockUnlock(mMailboxLock);
        return;
    }
    auto next = mState;
    if (route) {
        next.routeKnown = true;
        next.speakers = value == kInternalSpeaker;
        uint32_t state = 0;
        if (readNumber(mEngine, "IOAudioEngineState", state)) {
            next.engineKnown = true;
            next.engineRunning = state == kEngineRunning;
        }
    } else {
        next.engineKnown = true;
        next.engineRunning = value == kEngineRunning;
    }
    if (next.routeKnown != mState.routeKnown || next.speakers != mState.speakers ||
        next.engineKnown != mState.engineKnown || next.engineRunning != mState.engineRunning) {
        next.generation = mState.generation + 1;
        mState = next;
        mPending = true;
        signalWorkAvailable();
    }
    IOLockUnlock(mMailboxLock);
    if (engine)
        engine->release();
}

AudioEventState AudioEventSource::snapshot() {
    IOLockLock(mMailboxLock);
    auto state = mState;
    IOLockUnlock(mMailboxLock);
    return state;
}

bool AudioEventSource::unchanged(uint32_t generation) {
    return snapshot().generation == generation;
}

bool AudioEventSource::checkForWork() {
    IOLockLock(mMailboxLock);
    bool pending = mPending && mAttached;
    auto state = mState;
    mPending = false;
    IOLockUnlock(mMailboxLock);
    if (pending && mHandler)
        mHandler(owner, state);
    return false;
}

void AudioEventSource::free() {
    if (mAttached)
        detach();
    OSSafeReleaseNULL(mEngine);
    OSSafeReleaseNULL(mController);
    if (mMailboxLock)
        IOLockFree(mMailboxLock);
    IOEventSource::free();
}
}
