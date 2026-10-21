#include "Service/CirrusAudioFixupSupport.hpp"

bool CirrusAudioFixup::setupPowerManagement(IOService* provider) {
    PMinit();
    mPMInitialized = true;
    provider->joinPMtree(this);
    mPowerStates[0].version = 1;
    mPowerStates[1].version = 1;
    mPowerStates[1].capabilityFlags = kIOPMPowerOn | kIOPMDeviceUsable;
    mPowerStates[1].outputPowerCharacter = kIOPMPowerOn;
    mPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
    const IOReturn result = registerPowerDriver(this, mPowerStates, 2);
    setProperty("Cirrus_PM_Registration_Return", uint64_t(uint32_t(result)), 32);
    if (result != kIOReturnSuccess)
        CIRRUS_ERR("PM_REGISTRATION_FAILED operation=registerPowerDriver return=0x%08X; service will stop", result);
    return result == kIOReturnSuccess;
}

IOReturn CirrusAudioFixup::lifecycleAction(OSObject* owner, void* operation, void*, void*, void*) {
    auto* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (!self)
        return kIOReturnBadArgument;
    const uintptr_t action = reinterpret_cast<uintptr_t>(operation);
    if (action == 2) {
        const bool pending = self->mResumePending;
        self->mStartupPending = false;
        self->handlePowerChange(false);
        self->mResumePending = false;
        self->mStopping = true;
        if (pending)
            self->acknowledgeSetPowerState();
    } else if (!self->mStopping) {
        self->handlePowerChange(action != 0);
        if (self->mResumePending)
            return kIOReturnNotReady;
    }
    return kIOReturnSuccess;
}

IOReturn CirrusAudioFixup::setPowerState(unsigned long state, IOService* device) {
    if (device != this || state > 1)
        return IOPMAckImplied;
    CIRRUS_LOG("BOOT_FLOW PM_CHANGE_BEGIN state=%lu", state);
    if (mCommandGate) {
        IOReturn result = mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(state));
        if (result == kIOReturnNotReady) {
            // IOKit expects an acknowledgement timeout in microseconds.
            return 35000000;
        }
        if (result != kIOReturnSuccess)
            CIRRUS_ERR("PM gate failed: 0x%08X", result);
    }
    CIRRUS_LOG("BOOT_FLOW PM_CHANGE_END state=%lu", state);
    return IOPMAckImplied;
}

void CirrusAudioFixup::handlePowerChange(bool powered) {
    if (mStopping || (powered == mPowerAvailable && !(mResumePending && !powered)))
        return;
    if (!powered) {
        if (mAmpInterrupt)
            mAmpInterrupt->disable();
        if (mProbeTimer)
            mProbeTimer->cancelTimeout();

        if (mAmplifierManager.bound()) {
            auto stage = mAmplifierManager.stage();
            if (stage == cirrus::core::AmplifierStage::Ready || stage == cirrus::core::AmplifierStage::Playing) {
                IOReturn ret = mAmplifierManager.suspend();
                setProperty("Cirrus_Runtime_Suspend_Return", uint64_t(uint32_t(ret)), 32);
                if (ret != kIOReturnSuccess)
                    CIRRUS_ERR("AMP suspend cleanup failed: ret=0x%08X; runtime fault retained", ret);
            } else if (stage != cirrus::core::AmplifierStage::Faulted)
                releaseAmplifierDrivers();
        }
        suspendCommunication();
        mResumePending = false;
        mPowerAvailable = false;
        clearHdaCache();
        mHdaState.streamActive = mHdaState.observed = mHdaState.converterPrepared = false;
        mHdaState.lastDescriptor = 0xFF;
        mHdaState.lastStreamTag = 0;
        mHdaState.lastFormat = 0;
        setProperty("Cirrus_PM_Powered", uint64_t(uint32_t(0)), 32);
    } else {
        if (mStartupPending) {
            mPowerAvailable = true;
            if (mProbeTimer)
                mProbeTimer->setTimeoutMS(10);
            return;
        }
        if (!resumeCommunication()) {
            if (mResumePending && mProbeTimer)
                mProbeTimer->setTimeoutMS(10);
            else
                setProperty("Cirrus_SPI_Status", "WAKE_CONTROLLER_NOT_READY");
            return;
        }
        mPowerAvailable = true;
        mNeedsReinitialization = !bootArgEnabled("-cirrusro");
        mHdaState.topologyLogged = false;
        setProperty("Cirrus_PM_Powered", uint64_t(uint32_t(1)), 32);

        if (mNeedsReinitialization) {
            if (mAmplifierManager.bound() && mAmplifierManager.stage() == cirrus::core::AmplifierStage::Suspended) {
                IOReturn ret = mAmplifierManager.resume();
                setProperty("Cirrus_Runtime_Resume_Return", uint64_t(uint32_t(ret)), 32);
                if (ret != kIOReturnSuccess)
                    CIRRUS_ERR("AMP resume not ready: stage=%u ret=0x%08X; playback blocked", unsigned(mAmplifierManager.stage()), ret);
            } else
                bringUpAmplifiers();
            mNeedsReinitialization = false;
        }
        if (mAmpInterrupt && configureAmplifierInterrupt())
            mAmpInterrupt->enable();
        if (mProbeTimer)
            mProbeTimer->setTimeoutMS(50);
    }
    publishDriverVerdict();
}
