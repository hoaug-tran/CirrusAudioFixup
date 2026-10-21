#include "Service/CirrusAudioFixupSupport.hpp"

bool CirrusAudioFixup::setupAmplifierInterrupt() {
    setProperty("Cirrus_Amp_IRQ_Mode", "POLLING");
    const auto* wiring = mPlatformProfile ? mPlatformProfile->interruptWiring : nullptr;
    if (!bootArgEnabled("-cirrusirq")) {
        setProperty("Cirrus_Amp_IRQ_Fallback", "IRQ_NOT_REQUESTED");
        return false;
    }
    if (bootArgEnabled("-cirrusro") || mDebugPhaseHalted || !mPlatformAuthorized || !mProvider || !mWorkLoop ||
        !mAmplifierManager.bound()) {
        setProperty("Cirrus_Amp_IRQ_Fallback", "DRIVER_NOT_READY_OR_READ_ONLY");
        return false;
    }
    if (!wiring) {
        setProperty("Cirrus_Amp_IRQ_Fallback", "NO_VERIFIED_IRQ_WIRING");
        CIRRUS_LOG("IRQ unavailable: no verified board wiring; using polling");
        return false;
    }
    if (wiring->acpiNative) {
        auto* specifiers = OSDynamicCast(OSArray, mProvider->getProperty("IOInterruptSpecifiers"));
        auto* controllers = OSDynamicCast(OSArray, mProvider->getProperty("IOInterruptControllers"));
        if (!specifiers || !controllers || wiring->providerIndex >= specifiers->getCount() ||
            wiring->providerIndex >= controllers->getCount()) {
            setProperty("Cirrus_Amp_IRQ_Fallback", "ACPI_INTERRUPT_RESOURCE_MISSING");
            CIRRUS_ERR("IRQ index=%u missing ACPI interrupt resources; using polling", wiring->providerIndex);
            return false;
        }
    } else {
        auto* mode = OSDynamicCast(OSString, mProvider->getProperty("Interrupt Mode"));
        auto* pin = OSDynamicCast(OSNumber, mProvider->getProperty("gpioPin"));
        auto* irq = OSDynamicCast(OSNumber, mProvider->getProperty("gpioIRQ"));
        if (!mode || strcmp(mode->getCStringNoCopy(), "GPIO") || !pin || !irq || pin->unsigned32BitValue() != wiring->gpioPin ||
            irq->unsigned32BitValue() != wiring->gpioIRQ) {
            setProperty("Cirrus_Amp_IRQ_Fallback", "GPIO_INTERRUPT_WIRING_MISMATCH");
            CIRRUS_ERR("IRQ wiring mismatch: expected pin=%u IRQ=%u; using polling", wiring->gpioPin, wiring->gpioIRQ);
            return false;
        }
    }
    mAmpInterrupt = IOInterruptEventSource::interruptEventSource(this, amplifierInterruptReceived, mProvider, wiring->providerIndex);
    if (!mAmpInterrupt) {
        setProperty("Cirrus_Amp_IRQ_Fallback", "INTERRUPT_SOURCE_ALLOCATION_FAILED");
        CIRRUS_ERR("IRQ allocation failed; using polling");
        return false;
    }
    IOReturn irqReturn = mWorkLoop->addEventSource(mAmpInterrupt);
    if (irqReturn != kIOReturnSuccess) {
        setProperty("Cirrus_Amp_IRQ_Fallback", "INTERRUPT_SOURCE_ATTACH_FAILED");
        CIRRUS_ERR("IRQ attach failed: ret=0x%08X; using polling", irqReturn);
        OSSafeReleaseNULL(mAmpInterrupt);
        return false;
    }
    if (!configureAmplifierInterrupt()) {
        teardownAmplifierInterrupt();
        return false;
    }
    mAmpInterrupt->enable();
    setProperty("Cirrus_Amp_IRQ_Mode", wiring->name);
    setProperty("Cirrus_Amp_IRQ_Fallback", "NONE");
    return true;
}

bool CirrusAudioFixup::configureAmplifierInterrupt() {
    bool ok = mAmplifierManager.bound();
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        IOReturn ret = mAmplifierManager.configureInterrupt(i, true);
        if (ret != kIOReturnSuccess)
            CIRRUS_ERR("AMP IRQ configuration unavailable/failed: index=%lu ret=0x%08X; using polling", i, ret);
        ok = (ret == kIOReturnSuccess) && ok;
    }
    if (!ok) {
        for (size_t i = 0; i < mAmplifierManager.count(); ++i)
            mAmplifierManager.configureInterrupt(i, false);
        setProperty("Cirrus_Amp_IRQ_Mode", "POLLING_IRQ_SETUP_FAILED");
        setProperty("Cirrus_Amp_IRQ_Fallback", "DRIVER_INTERRUPT_CONFIGURATION_FAILED");
    }
    return ok;
}

void CirrusAudioFixup::amplifierInterruptReceived(OSObject* owner, IOInterruptEventSource* source, int) {
    auto* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (!self || self->mStopping || !self->mPowerAvailable)
        return;
    // A shared level interrupt must be serviced for both amps. On any fault
    // or I/O failure leave delivery disabled and latch the existing cleanup
    // policy. Do not acknowledge/release speaker protection automatically.
    source->disable();
    bool healthy = true;
    for (size_t i = 0; i < self->mAmplifierManager.count(); ++i) {
        const IOReturn result = self->mAmplifierManager.handleInterrupt(i);
        if (result != kIOReturnSuccess) {
            char key[64]{};
            snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_IRQ_Return", i);
            self->setProperty(key, uint64_t(uint32_t(result)), 32);
            CIRRUS_ERR("AMP IRQ handling failed: index=%lu return=0x%08X; delivery disabled", i, result);
        }
        healthy = (result == kIOReturnSuccess) && healthy;
    }
    if (!healthy) {
        self->setProperty("Cirrus_Amp_IRQ_Mode", "FAULT_LATCHED_IRQ_DISABLED");
        for (size_t i = 0; i < self->mAmplifierManager.count(); ++i)
            self->mAmplifierManager.cancelPlayback(i);
    }
    // No enabled protection source was observed. Fall back instead of
    // repeatedly re-enabling an unexplained asserted shared level line.
    if (healthy)
        self->setProperty("Cirrus_Amp_IRQ_Mode", "POLLING_UNEXPLAINED_IRQ_DISABLED");
    self->mAmplifierManager.refreshPlaybackState();
    if (!healthy || self->publishRuntimeStatus())
        self->publishDriverVerdict();
    CIRRUS_LOG("AMP IRQ disabled: reason=%s; polling remains active", healthy ? "UNEXPLAINED_LEVEL" : "FAULT_OR_TRANSFER_ERROR");
}

void CirrusAudioFixup::teardownAmplifierInterrupt() {
    if (!mAmpInterrupt)
        return;
    mAmpInterrupt->disable();
    if (mWorkLoop)
        mWorkLoop->removeEventSource(mAmpInterrupt);
    OSSafeReleaseNULL(mAmpInterrupt);
}

bool CirrusAudioFixup::setupProbeTimer() {
    // A separate loop lets provider matching and PM progress during startup.
    mWorkLoop = IOWorkLoop::workLoop();

    if (!mWorkLoop) {
        CIRRUS_ERR("EVENT_SETUP_FAILED operation=workLoop reason=NO_MEMORY");
        return false;
    }

    mProbeTimer = IOTimerEventSource::timerEventSource(this, probeTimerFired);
    if (!mProbeTimer) {
        CIRRUS_ERR("EVENT_SETUP_FAILED operation=timerEventSource reason=NO_MEMORY");
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    IOReturn eventReturn = mWorkLoop->addEventSource(mProbeTimer);
    if (eventReturn != kIOReturnSuccess) {
        CIRRUS_ERR("EVENT_SETUP_FAILED operation=addTimer return=0x%08X", eventReturn);
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    mCommandGate = IOCommandGate::commandGate(this);
    eventReturn = mCommandGate ? mWorkLoop->addEventSource(mCommandGate) : kIOReturnNoMemory;
    if (eventReturn != kIOReturnSuccess) {
        CIRRUS_ERR("EVENT_SETUP_FAILED operation=%s return=0x%08X", mCommandGate ? "addCommandGate" : "commandGate", eventReturn);
        OSSafeReleaseNULL(mCommandGate);
        mWorkLoop->removeEventSource(mProbeTimer);
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }
    setProperty("CirrusTimerCreated", kOSBooleanTrue);
    return true;
}

void CirrusAudioFixup::scheduleReadOnlyProbe(UInt32 delayMs) {
    CIRRUS_LOG("read-only probe scheduled in %u ms", delayMs);
    mProbeTimer->setTimeoutMS(delayMs);
}

void CirrusAudioFixup::probeTimerFired(OSObject* owner, IOTimerEventSource* sender) {
    (void)sender;
    CirrusAudioFixup* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (self) {
        if (self->mStopping || self->mStartupFailed)
            return;
        if (!self->mPowerAvailable && !self->mResumePending)
            return;
        if (self->mStartupPending) {
            self->continueStartup();
            return;
        }
        if (self->mResumePending) {
            self->handlePowerChange(true);
            if (self->mResumePending)
                self->mProbeTimer->setTimeoutMS(10);
            else
                self->acknowledgeSetPowerState();
            return;
        }
        if (!self->mPowerAvailable)
            return;
        self->setProperty("CirrusTimerFired", kOSBooleanTrue);
        if (self->bootArgEnabled("-cirrusro")) {
            self->runReadOnlyProbe();
        } else {
            self->runBackgroundMonitor();
        }
    }
}

void CirrusAudioFixup::runReadOnlyProbe() {
    CIRRUS_LOG("BOOT_FLOW READ_ONLY_PROBE_BEGIN count=%lu", mAmplifierCount);

    for (size_t i = 0; i < mAmplifierCount; ++i) {
        CIRRUS_LOG("BOOT_FLOW AMP_INSPECT_BEGIN index=%lu", i);
        probeAmplifier(mAmplifiers[i]);
        CIRRUS_LOG("BOOT_FLOW AMP_INSPECT_END index=%lu", i);
    }

    publishStatistics();

    if (bootArgEnabled("-cirrusdumptrace")) {
        dumpTraceBuffer();
    }

    CIRRUS_LOG("BOOT_FLOW READ_ONLY_PROBE_END");
}

bool CirrusAudioFixup::supportedHdaFormat(uint16_t format) { return cirrus::platform::hda::HDAStreamWatcher::isFormatSupported(format); }

void CirrusAudioFixup::clearHdaCache() {
    teardownAudioEvents();
    OSSafeReleaseNULL(mHdaMap);
    OSSafeReleaseNULL(mHdaPci);
    mHdaStatus = nullptr;
    mHdaState.topologyLogged = false;
}

bool CirrusAudioFixup::synchronizeHdaStream() {
    const auto previous = mHdaState;
    const bool wasPrepared = mHdaState.converterPrepared;
    auto publishStatus = [&](const char* text) {
        if (mHdaStatus && strcmp(mHdaStatus, text) == 0)
            return;
        OSString* status = OSString::withCString(text);
        if (status) {
            setProperty("Cirrus_HDA_Status", status);
            mHdaStatus = text;
            status->release();
        }
    };
    mHdaState.observed = false;
    mHdaState.streamActive = mHdaState.converterPrepared = false;
    mHdaState.lastDescriptor = 0xFF;
    mHdaState.lastStreamTag = 0;
    mHdaState.lastFormat = 0;
    if (!mPowerAvailable || mStopping)
        return false;
    if (mHdaPci && (mHdaPci->isInactive() || mHdaPci->configRead16(kIOPCIConfigVendorID) == 0xFFFF))
        clearHdaCache();
    const bool usingCachedController = mHdaPci != nullptr;
    IOService* audioCtrl = usingCachedController ? mHdaPci : getAudioController();
    if (!audioCtrl) {
        if (mHdaState.missCount < 20 && ++mHdaState.missCount == 20) {
            for (size_t i = 0; i < mAmplifierCount; ++i) {
                setDiagnosticStage(mAmplifiers[i], STAGE_HDA_DETECT);
                recordDiagnosticFailure(mAmplifiers[i], DIAG_HDA_CONTROLLER, 0, 1, 0, kIOReturnNotFound, false);
            }
        }
        publishStatus(mHdaState.missCount >= 20 ? "MISSING_AFTER_20_POLLS" : "MISSING");
        return false;
    }
    if (mHdaState.missCount != 0) {
        CIRRUS_LOG("HDA controller discovered after %u missed polls", mHdaState.missCount);
        mHdaState.missCount = 0;
    }

    IOPCIDevice* pciDev =
#if defined(KERNEL)
        audioCtrl ? static_cast<IOPCIDevice*>(audioCtrl->metaCast("IOPCIDevice")) : nullptr
#else
        OSDynamicCast(IOPCIDevice, audioCtrl)
#endif
        ;
    if (!pciDev) {
        publishStatus("NOT_A_PCI_CONTROLLER");
        if (usingCachedController)
            clearHdaCache();
        else
            audioCtrl->release();
        return false;
    }
    if (pciDev->isInactive() || pciDev->configRead16(kIOPCIConfigVendorID) == 0xFFFF) {
        publishStatus("CONTROLLER_UNAVAILABLE");
        if (usingCachedController)
            clearHdaCache();
        else
            audioCtrl->release();
        return false;
    }

    IOMemoryMap* map = mHdaMap ? mHdaMap : pciDev->mapDeviceMemoryWithRegister(0x10);
    mHdaPci = pciDev;
    mHdaMap = map;
    if (!map || map->getLength() < 0x80) {
        publishStatus("INVALID_BAR_MAPPING");
        clearHdaCache();
        return false;
    }

    volatile uint8_t* base = (volatile uint8_t*)map->getVirtualAddress();
    mHdaState.observed = base != nullptr;
    if (!base) {
        publishStatus("INVALID_BAR_ADDRESS");
        clearHdaCache();
        return false;
    }

    bool outputRunning = false;
    uint8_t activeStream = 0;
    uint16_t activeFormat = 0;
    uint8_t activeDescriptor = 0xFF;
    unsigned runningOutputs = 0;

    uint16_t gcap = *(volatile uint16_t*)(base + 0x00);
    uint32_t gctl = *(volatile uint32_t*)(base + 0x08);
    if (gcap == 0xFFFF || gctl == 0xFFFFFFFF || !(gctl & 1)) {
        publishStatus("CONTROLLER_NOT_READY");
        mHdaState.observed = false;
        clearHdaCache();
        return false;
    }
    uint8_t inputStreams = (gcap >> 8) & 0x0F;
    uint8_t outputStreams = (gcap >> 12) & 0x0F;
    uint8_t bidirectionalStreams = (gcap >> 3) & 0x1F;
    uint8_t firstOutput = inputStreams;
    uint8_t descriptorCount = inputStreams + outputStreams + bidirectionalStreams;
    if (map->getLength() < 0x80U + uint32_t(descriptorCount) * 0x20U) {
        publishStatus("TRUNCATED_STREAM_DESCRIPTORS");
        mHdaState.observed = false;
        clearHdaCache();
        return false;
    }

    if (!mHdaState.topologyLogged && pciDev) {
        CIRRUS_LOG("HDA controller %s %04X:%04X GCAP=0x%04X ISS=%u OSS=%u BSS=%u; speaker converter/pin route is unverified",
                   pciDev->getName() ? pciDev->getName() : "unnamed", pciDev->configRead16(kIOPCIConfigVendorID),
                   pciDev->configRead16(kIOPCIConfigDeviceID), gcap, inputStreams, outputStreams, bidirectionalStreams);
        mHdaState.topologyLogged = true;
    }

    for (uint8_t index = firstOutput; index < descriptorCount; ++index) {
        bool fixedOutput = index < (uint8_t)(firstOutput + outputStreams);
        volatile uint8_t* sd = base + 0x80 + (index * 0x20);
        uint32_t sdCtl = *(volatile uint32_t*)(sd + 0x00);
        bool bidirectionalOutput = !fixedOutput && ((sdCtl & (1U << 19)) != 0);
        if ((fixedOutput || bidirectionalOutput) && (sdCtl & 0x00000003) == 0x00000002) {
            uint8_t candidateStream = (sdCtl >> 20) & 0x0F;
            if (candidateStream != 0) {
                activeStream = candidateStream;
                activeFormat = *(volatile uint16_t*)(sd + 0x12);
                activeDescriptor = index;
                outputRunning = true;
                ++runningOutputs;
            }
        }
    }

    if (runningOutputs > 1) {
        publishStatus("AMBIGUOUS_OUTPUT_STREAMS");
        return false;
    }
    bool converterPrepared = outputRunning && activeStream != 0 && supportedHdaFormat(activeFormat);
    bool streamActive = converterPrepared;
    const bool streamChanged = previous.lastDescriptor != activeDescriptor || previous.lastStreamTag != activeStream ||
                               previous.lastFormat != activeFormat || previous.observed != mHdaState.observed;
    if (streamChanged) {
        char text[192]{};
        snprintf(text, sizeof(text), "descriptor=%u tag=%u format=0x%04X rate_hz=%u bits=%u channels=%u compatible=%u",
                 activeDescriptor, activeStream, activeFormat,
                 outputRunning ? cirrus::platform::hda::HDAStreamWatcher::decodeSampleRate(activeFormat) : 0,
                 outputRunning ? cirrus::platform::hda::HDAStreamWatcher::decodeBitsPerSample(activeFormat) : 0,
                 outputRunning ? unsigned((activeFormat & 15) + 1) : 0, unsigned(converterPrepared));
        if (auto* summary = OSString::withCString(text)) {
            setProperty("Cirrus_HDA_Last_Stream", summary);
            summary->release();
        }
    }

    // Record format rejection once per stream change.
    if (streamChanged && outputRunning && activeStream != 0 && !supportedHdaFormat(activeFormat)) {
        for (size_t i = 0; i < mAmplifierCount; ++i) {
            setDiagnosticStage(mAmplifiers[i], STAGE_HDA_DETECT);
            recordDiagnosticFailure(mAmplifiers[i], DIAG_HDA_STREAM_FORMAT, activeDescriptor, 1, activeFormat, kIOReturnSuccess, false);
        }
    }

    if (converterPrepared && !wasPrepared) {
        CIRRUS_LOG("HDA output stream observed: descriptor=%u stream=%u format=0x%04X; speaker route unverified", activeDescriptor,
                   activeStream, activeFormat);
    }

    if (!converterPrepared && wasPrepared) {
        CIRRUS_LOG("HDA playback cleanup: no unique compatible running output stream");
    }
    publishStatus(converterPrepared ? "RUNNING_ROUTE_UNCONFIRMED" : outputRunning ? "UNSUPPORTED_STREAM_FORMAT" : "IDLE");
    mHdaState.converterPrepared = converterPrepared;
    mHdaState.streamActive = streamActive;
    mHdaState.lastDescriptor = activeDescriptor;
    mHdaState.lastStreamTag = activeStream;
    mHdaState.lastFormat = activeFormat;
    return streamActive;
}

bool CirrusAudioFixup::setupAudioEvents() {
    if (mAudioEvents) {
        mAudioEvents->refreshRoute();
        mAudioState = mAudioEvents->snapshot();
        setProperty("Cirrus_Audio_Event_Mode", mAudioState.routeKnown ? "IOAUDIO_FAMILY_EVENTS" : "IOAUDIO_HOOKS_WAITING_FOR_OUTPUT");
        return true;
    }
    if (!mHdaPci || !cirrus::platform::hda::AudioEventSource::hooksReady())
        return false;
    auto* source = cirrus::platform::hda::AudioEventSource::create(this, mHdaPci, audioEventReceived);
    if (!source)
        return false;
    if (mWorkLoop->addEventSource(source) != kIOReturnSuccess) {
        source->release();
        return false;
    }
    if (!source->attach()) {
        mWorkLoop->removeEventSource(source);
        source->release();
        return false;
    }
    mAudioEvents = source;
    mAudioState = source->snapshot();
    setProperty("Cirrus_Audio_Event_Mode", mAudioState.routeKnown ? "IOAUDIO_FAMILY_EVENTS" : "IOAUDIO_HOOKS_WAITING_FOR_OUTPUT");
    return true;
}

void CirrusAudioFixup::teardownAudioEvents() {
    if (mAudioEvents) {
        mAudioEvents->detach();
        if (mWorkLoop)
            mWorkLoop->removeEventSource(mAudioEvents);
        OSSafeReleaseNULL(mAudioEvents);
    }
    mAudioState = {};
    mAudioPrepareRetries = 0;
}

void CirrusAudioFixup::audioEventReceived(OSObject* owner, const cirrus::platform::hda::AudioEventState& state) {
    auto* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (!self || self->mStopping || !self->mPowerAvailable)
        return;
    self->mAudioState = state;
    if (state.routeKnown)
        self->setProperty("Cirrus_Audio_Event_Mode", "IOAUDIO_FAMILY_EVENTS");
    self->mAudioPrepareRetries = state.routeKnown && state.speakers ? 40 : 0;
    if (self->mProbeTimer)
        self->mProbeTimer->cancelTimeout();
    self->runBackgroundMonitor();
}

bool CirrusAudioFixup::retryPlatformIdentity() {
    if (!mPlatformIdentityPending)
        return false;
    resolvePlatformProfile();
    if (!mPlatformSubsystemId) {
        if (playbackTimeMilliseconds() < mPlatformIdentityDeadlineMs) {
            if (mProbeTimer)
                mProbeTimer->setTimeoutMS(100);
            return true;
        }
        mPlatformIdentityPending = false;
        mAttachment.amplifier = "BOARD_IDENTITY_TIMEOUT";
        setProperty("Cirrus_Board_Identity_Status", "HDA_PCI_IDENTITY_TIMEOUT");
        recordAttachmentFailure("POLICY", "HDA_PCI_IDENTITY_TIMEOUT", kIOReturnTimeout);
        publishDriverVerdict();
        return true;
    }
    mPlatformIdentityPending = false;
    setProperty("Cirrus_Board_Identity_Status", mPlatformAuthorized ? "VERIFIED" : "REJECTED");
    CIRRUS_LOG("BOARD_IDENTITY_RESOLVED board=0x%08X authorized=%u", mPlatformSubsystemId, unsigned(mPlatformAuthorized));
    if (!mPlatformAuthorized) {
        mAttachment.amplifier = "UNSUPPORTED_BOARD";
        recordAttachmentFailure("POLICY", "BOARD_NOT_AUTHORIZED", kIOReturnNotPermitted);
        publishDriverVerdict();
        return true;
    }
    teardownCommunication();
    if (!setupCommunication(mProvider)) {
        publishDriverVerdict();
        return true;
    }
    if (mNativeSPI.powerPending()) {
        mStartupPending = true;
        setProperty("Cirrus_Driver_Verdict", "WAITING_CONTROLLER_POWER");
        if (mProbeTimer)
            mProbeTimer->setTimeoutMS(10);
        return true;
    }
    if (!mPlatformAuthorized) {
        publishDriverVerdict();
        return true;
    }
    if (!performPlatformHardwareReset() && mNativeSPI.bound()) {
        publishDriverVerdict();
        return true;
    }
    IOSleep(15);
    if (mNativeSPI.bound())
        detectNativeSPIAmplifiers();
    else
        detectAmplifiers();
    bringUpAmplifiers();
    setupAmplifierInterrupt();
    if (mProbeTimer)
        mProbeTimer->setTimeoutMS(50);
    return true;
}

bool CirrusAudioFixup::retryBringUpResources() {
    if (mAmplifierManager.stage() != cirrus::core::AmplifierStage::Waiting)
        return false;
    mAmplifierManager.retryBringUp(playbackTimeMilliseconds());
    publishDriverVerdict();
    const bool waiting = mAmplifierManager.stage() == cirrus::core::AmplifierStage::Waiting;
    if (waiting && mProbeTimer)
        mProbeTimer->setTimeoutMS(100);
    return waiting;
}

void CirrusAudioFixup::runBackgroundMonitor() {
    if (mStopping || !mPowerAvailable || mNeedsReinitialization || mDebugPhaseHalted)
        return;
    if (retryPlatformIdentity())
        return;
    // Diagnostic-only/failed attachments do not acquire HDA mappings or rearm
    // polling. Pending resources still have a bound Waiting runtime.
    if (!mPlatformAuthorized || !mAmplifierManager.bound() || mAmplifierManager.stage() == cirrus::core::AmplifierStage::Faulted ||
        mAmplifierManager.stage() == cirrus::core::AmplifierStage::Suspended)
        return;
    if (mStopping || !mPowerAvailable || mNeedsReinitialization || mDebugPhaseHalted)
        return;
    if (retryBringUpResources())
        return;
    bool hdaStreamActive = synchronizeHdaStream();
    setupAudioEvents();
    if (mAudioEvents)
        mAudioState = mAudioEvents->snapshot();
    const uint32_t routeGeneration = mAudioState.generation;
    const bool eventRoute = mAudioState.routeKnown;
    // A DMA stream may belong to headphones/HDMI. An unknown output is not
    // permission to unmute board speakers, even when its format is compatible.
    const bool routePermitsPlayback = eventRoute && mAudioState.speakers && mAudioState.engineKnown && mAudioState.engineRunning;
    setProperty("Cirrus_Playback_Route_Policy", !eventRoute ? "UNKNOWN_OUTPUT_ROUTE"
                : !mAudioState.speakers ? "NON_SPEAKER_OUTPUT"
                : !mAudioState.engineKnown ? "UNKNOWN_OUTPUT_ENGINE"
                : !routePermitsPlayback ? "OUTPUT_ENGINE_STOPPED" : "SPEAKER_OUTPUT_SELECTED");
    bool hasAudio = mHdaState.observed && hdaStreamActive && routePermitsPlayback;
    if (!mAmplifierManager.bound() || (mAmplifierManager.stage() != cirrus::core::AmplifierStage::Ready &&
                                       mAmplifierManager.stage() != cirrus::core::AmplifierStage::Playing))
        return;
    for (size_t i = 0; i < mAmplifierManager.count(); ++i)
        if (mAmplifierManager.playbackFaulted(i))
            hasAudio = false;
    uint64_t nowMs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowMs);
    nowMs /= 1000000;
    cirrus::core::PlaybackContext context{
        hasAudio, eventRoute, routePermitsPlayback, hdaStreamActive, routeGeneration, mHdaState.lastStreamTag, mHdaState.lastFormat, nowMs};
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        IOReturn ret = mAmplifierManager.runtimeTick(i, context);
        if (ret != kIOReturnSuccess) {
            setProperty("Cirrus_Runtime_Tick_Return", uint64_t(uint32_t(ret)), 32);
            setProperty("Cirrus_Runtime_Tick_Index", uint64_t(uint32_t(i)), 32);
            CIRRUS_ERR("AMP runtime failure: index=%lu ret=0x%08X stage=%u; group playback blocked", i, ret,
                       unsigned(mAmplifierManager.stage()));
            break;
        }
    }
    mAmplifierManager.refreshPlaybackState();
    bool allPrepared = hasAudio, anyFault = false;
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        allPrepared &= mAmplifierManager.playbackPrepared(i);
        anyFault |= mAmplifierManager.playbackFaulted(i);
    }
    if (allPrepared && mAmplifierManager.stage() == cirrus::core::AmplifierStage::Ready) {
        bool valid = synchronizeHdaStream() && mHdaState.observed;
        for (size_t i = 0; i < mAmplifierManager.count() && valid; ++i)
            valid = mAmplifierManager.validatePlayback(i, context);
        if (valid) {
            IOReturn ret = mAmplifierManager.startPlayback();
            valid = ret == kIOReturnSuccess;
            setProperty("Cirrus_Runtime_Start_Return", uint64_t(uint32_t(ret)), 32);
            if (!valid && mAmplifierManager.stage() == cirrus::core::AmplifierStage::Faulted)
                CIRRUS_ERR("AMP group start failed: ret=0x%08X; peers rolled back", ret);
        }
        // A route can change while register operations sleep. Revalidate the
        // complete group after its final commit and cancel every participant.
        if (valid && mAudioEvents)
            valid = mAudioEvents->unchanged(routeGeneration);
        if (!valid)
            for (size_t i = 0; i < mAmplifierManager.count(); ++i)
                mAmplifierManager.cancelPlayback(i);
    } else if (anyFault) {
        for (size_t i = 0; i < mAmplifierManager.count(); ++i)
            mAmplifierManager.cancelPlayback(i);
    }
    mAmplifierManager.refreshPlaybackState();
    if (publishRuntimeStatus())
        publishDriverVerdict();
    bool pending = false, needsPreparation = false;
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        pending |= mAmplifierManager.playbackPending(i);
        needsPreparation |= mAmplifierManager.playbackNeedsPreparation(i);
    }
    if (mProbeTimer) {
        if (pending) {
            mProbeTimer->setTimeoutMS(1);
            return;
        }
        if (eventRoute) {
            if (routePermitsPlayback && needsPreparation && mAudioPrepareRetries > 0) {
                --mAudioPrepareRetries;
                mProbeTimer->setTimeoutMS(5);
            } else if (mAmplifierManager.stage() == cirrus::core::AmplifierStage::Playing && routePermitsPlayback)
                mProbeTimer->setTimeoutMS(100);
            else if (routePermitsPlayback)
                // Observe format changes without an IOAudio event. A waiting
                // stream cannot start AMP preparation until its format is valid.
                mProbeTimer->setTimeoutMS(250);
            return;
        }
        mProbeTimer->setTimeoutMS(mHdaState.streamActive ? 100 : 50);
    }
}

IOService* CirrusAudioFixup::audioController() {
    OSDictionary* hdaMatching = serviceMatching("AppleHDAController");
    if (hdaMatching) {
        OSIterator* hdaIter = getMatchingServices(hdaMatching);
        hdaMatching->release();
        if (hdaIter) {
            IOPCIDevice* bestHdaPci = nullptr;
            int bestHdaScore = -1000;
            IORegistryEntry* entry;
            while ((entry = OSDynamicCast(IORegistryEntry, hdaIter->getNextObject()))) {
                IORegistryEntry* parent = entry;
                for (unsigned depth = 0; parent && depth < 8; ++depth) {
                    IOPCIDevice* pci =
#if defined(KERNEL)
                        parent ? static_cast<IOPCIDevice*>(parent->metaCast("IOPCIDevice")) : nullptr
#else
                        OSDynamicCast(IOPCIDevice, parent)
#endif
                        ;
                    if (pci) {
                        const char* name = pci->getName();
                        uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
                        int score = 0;
                        if (name && strcmp(name, "HDEF") == 0)
                            score += 100;
                        else if (name && strcmp(name, "HDAS") == 0)
                            score += 80;
                        else if (name && strcmp(name, "HDAU") == 0)
                            score -= 100;
                        if (vendor == 0x10DE)
                            score -= 80;
                        else
                            score += 50;
                        if (score > bestHdaScore) {
                            bestHdaScore = score;
                            bestHdaPci = pci;
                        }
                        break;
                    }
                    parent = parent->getParentEntry(gIOServicePlane);
                }
            }
            if (bestHdaPci && bestHdaScore >= 0)
                bestHdaPci->retain();
            hdaIter->release();
            if (bestHdaPci && bestHdaScore >= 0)
                return bestHdaPci;
        }
    }

    OSDictionary* matching = serviceMatching("IOPCIDevice");
    if (!matching)
        return nullptr;

    OSIterator* iter = getMatchingServices(matching);
    matching->release();
    if (!iter)
        return nullptr;

    IOService* service;
    IOService* bestController = nullptr;
    int bestScore = -1;

    while ((service = OSDynamicCast(IOService, iter->getNextObject()))) {
        uint32_t pciVendor = 0;
        IOPCIDevice* pci =
#if defined(KERNEL)
            service ? static_cast<IOPCIDevice*>(service->metaCast("IOPCIDevice")) : nullptr
#else
            OSDynamicCast(IOPCIDevice, service)
#endif
            ;
        if (pci) {
            pciVendor = pci->configRead16(kIOPCIConfigVendorID);
        } else {
            OSData* venData = OSDynamicCast(OSData, service->getProperty("vendor-id"));
            if (venData && venData->getLength() >= 4) {
                pciVendor = *((uint32_t*)venData->getBytesNoCopy()) & 0xFFFF;
            } else if (venData && venData->getLength() >= 2) {
                pciVendor = *((uint16_t*)venData->getBytesNoCopy());
            }
        }
        if (pciVendor == 0xFFFF || pciVendor == 0xFFFFFFFF || pciVendor == 0) {
            continue;
        }

        int score = 0;
        OSData* classCodeData = OSDynamicCast(OSData, service->getProperty("class-code"));
        if (classCodeData && classCodeData->getLength() >= 3) {
            const uint8_t* bytes = (const uint8_t*)classCodeData->getBytesNoCopy();
            if (bytes[2] == 0x04 && bytes[1] == 0x03) {
                score += 50;
            }
        }

        const char* name = service->getName();
        if (name) {
            if (strcmp(name, "HDEF") == 0)
                score += 50;
            else if (strcmp(name, "HDAS") == 0)
                score += 40;
            else if (strcmp(name, "HDAU") == 0)
                score -= 100;
        }

        if (pciVendor == 0x10DE)
            score -= 50;
        else if (pciVendor == 0x1022 || pciVendor == 0x8086)
            score += 30;

        if (score >= 40 && score > bestScore) {
            bestScore = score;
            bestController = service;
        }
    }

    iter->release();
    if (bestScore >= 40 && bestController) {
        bestController->retain();
        return bestController;
    }
    return nullptr;
}

IOService* CirrusAudioFixup::getAudioController() { return audioController(); }
