#include "Service/CirrusAudioFixupSupport.hpp"

OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)
bool gCirrusDebug = false;

bool CirrusAudioFixup::init(OSDictionary* properties) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = cirrus::support::kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg");
#if defined(KERNEL)
    publishMatchingEvent("INIT_ENTERED", nullptr, kIOReturnSuccess);
#endif
    if (!super::init(properties)) {
        CIRRUS_ERR("super::init failed");
#if defined(KERNEL)
        publishMatchingEvent("INIT_SUPER_FAILED", nullptr, kIOReturnError);
#endif
        return false;
    }

    if (gCirrusDebug || bootArgEnabled("-cirrusdumptrace")) {
        mTraceLock = IOLockAlloc();
        if (!mTraceLock)
            CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_lock return=NO_MEMORY; continuing without transfer history");
        initTraceBuffer();
    }
    setProperty("Cirrus_Trace_Enabled", mTraceLock ? kOSBooleanTrue : kOSBooleanFalse);

    setProperty("CirrusReachedInit", kOSBooleanTrue);
    CIRRUS_LOG("init");
    return true;
}

IOService* CirrusAudioFixup::probe(IOService* provider, SInt32* score) {
    IOService* result = super::probe(provider, score);
#if defined(KERNEL)
    publishMatchingEvent("PROBE", provider, result ? kIOReturnSuccess : kIOReturnUnsupported);
#endif

    setProperty("CirrusReachedProbe", kOSBooleanTrue);

    CIRRUS_LOG("probe provider=%s class=%s score=%d", provider ? provider->getName() : "null",
               provider ? provider->getMetaClass()->getClassName() : "null", score ? static_cast<int>(*score) : -1);

    if (score) {
        *score += 9000;
    }

    return result;
}

bool CirrusAudioFixup::start(IOService* provider) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = cirrus::support::kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg");

    setProperty("Author", "Tran Kinh Hoang (hoaugtr)");
    setProperty("Cirrus_Build_Configuration", cirrus::support::kBuildConfiguration);
    setProperty("Cirrus_Diagnostic_Schema", cirrus::support::kDiagnosticSchema);
    setProperty("Cirrus_Debug_Enabled", gCirrusDebug ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Cirrus_Audio_Event_Mode", "HDA_TIMER_FALLBACK");
    IOLog("CirrusAudioFixup: %s build; verbose logging %s\n", cirrus::support::kBuildConfiguration, gCirrusDebug ? "enabled" : "disabled");
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("Author: Tran Kinh Hoang (hoaugtr)");
    CIRRUS_LOG("start");
#if defined(KERNEL)
    publishBootCheckpoint(this, "START_ENTERED");
    publishMatchingEvent("START_ENTERED", provider, kIOReturnSuccess);
#endif

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
#if defined(KERNEL)
        publishMatchingEvent("START_SUPER_FAILED", provider, kIOReturnError);
#endif
        return false;
    }

    mProvider = provider;
    logProviderInfo(provider);
    dumpProviderProperties(provider);
    CIRRUS_LOG("BOOT_FLOW PROFILE_BEGIN");
    resolvePlatformProfile();
    CIRRUS_LOG("BOOT_FLOW PROFILE_END");

    if (!registerAmplifierDrivers()) {
        recordAttachmentFailure("REGISTRATION", "DRIVER_REGISTRY_REJECTED", kIOReturnBadArgument);
        mProvider = nullptr;
        super::stop(provider);
        return false;
    }
    char requestedPhase[32]{};
    if (PE_parse_boot_argn("-cirrusphase", requestedPhase, sizeof(requestedPhase))) {
        requestedPhase[sizeof(requestedPhase) - 1] = 0;
        setProperty("Cirrus_Debug_Phase", requestedPhase);
        const bool valid = mDriverRegistry.acceptsDebugPhase(mPlatformProfile ? mPlatformProfile->acpiHid : nullptr, requestedPhase);
        if (!valid) {
            recordAttachmentFailure("DEBUG_POLICY", "INVALID_DEBUG_PHASE", kIOReturnBadArgument);
            CIRRUS_ERR("Unsupported -cirrusphase=%s; initialization rejected", requestedPhase);
            mProvider = nullptr;
            super::stop(provider);
            return false;
        }
    }

    if (!setupProbeTimer()) {
#if defined(KERNEL)
        publishBootCheckpoint(this, "EVENT_SETUP_FAILED", kIOReturnNoMemory);
        retainFailedServiceEvidence(this);
#endif
        mProvider = nullptr;
        super::stop(provider);
        return false;
    }
    CIRRUS_LOG("BOOT_FLOW COMMUNICATION_BEGIN");
#if defined(KERNEL)
    publishBootCheckpoint(this, "COMMUNICATION_BEGIN");
#endif
    const bool communicationReady = setupCommunication(provider);
    CIRRUS_LOG("BOOT_FLOW COMMUNICATION_END ready=%u", unsigned(communicationReady));
#if defined(KERNEL)
    publishBootCheckpoint(this, "COMMUNICATION_END", communicationReady ? kIOReturnSuccess : kIOReturnError);
#endif
    if (!communicationReady) {
#if defined(KERNEL)
        retainFailedServiceEvidence(this);
        publishMatchingEvent("START_COMMUNICATION_FAILED", provider, kIOReturnError);
#endif
        stop(provider);
        return false;
    }
    if (mNativeSPI.powerPending()) {
        mStartupPending = true;
        setProperty("Cirrus_Driver_Verdict", "WAITING_CONTROLLER_POWER");
#if defined(KERNEL)
        publishBootCheckpoint(this, "START_WAITING_CONTROLLER_POWER");
#endif
        registerService();
        mProbeTimer->setTimeoutMS(10);
        return true;
    }
    if (!finishStartup()) {
        stop(provider);
        return false;
    }
    return true;
}

void CirrusAudioFixup::continueStartup() {
    // Keep the same power request alive. Reset and identity probing start only
    // after every pending controller has finished binding.
    const auto result = mNativeSPI.continueBind();
    if (mNativeSPI.powerPending()) {
        mProbeTimer->setTimeoutMS(10);
        return;
    }
    mStartupPending = false;
    if (result == kIOReturnSuccess && setupCommunication(mProvider) && finishStartup())
        return;
    mStartupFailed = true;
    setProperty("Cirrus_Driver_Verdict", "STARTUP_FAILED");
    if (result != kIOReturnSuccess) {
        const auto& failure = mNativeSPI.failure();
        recordAttachmentFailure("COMMUNICATION", failure.reason, result, uint32_t(failure.index), "CONTROLLER_SLOT");
    }
#if defined(KERNEL)
    publishBootCheckpoint(this, "DEFERRED_START_FAILED", result == kIOReturnSuccess ? kIOReturnError : result);
    retainFailedServiceEvidence(this);
#endif
    teardownCommunication();
}

bool CirrusAudioFixup::finishStartup() {
    auto* provider = mProvider;
    mPlatformIdentityPending = !mPlatformSubsystemId && mPlatformProfile->allowAutomaticInitialization && !bootArgEnabled("-cirrusro");
    if (mPlatformIdentityPending) {
        mPlatformIdentityDeadlineMs = playbackTimeMilliseconds() + 30000;
        mAttachment.amplifier = "WAITING_BOARD_IDENTITY";
        setProperty("Cirrus_Board_Identity_Status", "WAITING_HDA_PCI_IDENTITY");
        setProperty("Cirrus_Driver_Verdict", "WAITING_BOARD_IDENTITY");
        CIRRUS_LOG("BOARD_IDENTITY_WAIT profile=%s deadline_ms=%llu; no reset or amplifier initialization", mPlatformProfile->name,
                   mPlatformIdentityDeadlineMs);
        publishAttachmentStatus();
    } else {
        CIRRUS_LOG("BOOT_FLOW RESET_POLICY_BEGIN");
#if defined(KERNEL)
        publishBootCheckpoint(this, "RESET_POLICY_BEGIN");
#endif
        bool hardwareResetVerified = performPlatformHardwareReset();
        CIRRUS_LOG("BOOT_FLOW RESET_POLICY_END policy_ok=%u", unsigned(hardwareResetVerified));
#if defined(KERNEL)
        publishBootCheckpoint(this, "RESET_POLICY_END", hardwareResetVerified ? kIOReturnSuccess : kIOReturnNotReady);
#endif
        if (!hardwareResetVerified) {
            if (mNativeSPI.bound()) {
#if defined(KERNEL)
                retainFailedServiceEvidence(this);
#endif
                teardownCommunication();
                return false;
            }
            CIRRUS_LOG("Platform hardware reset not asserted or verified; continuing with ACPI and I2C soft reset");
        }

        IOSleep(15);
        CIRRUS_LOG("BOOT_FLOW AMP_DISCOVERY_BEGIN");
#if defined(KERNEL)
        publishBootCheckpoint(this, "AMP_DISCOVERY_BEGIN");
#endif
        if (mNativeSPI.bound())
            detectNativeSPIAmplifiers();
        else
            detectAmplifiers();
        CIRRUS_LOG("BOOT_FLOW AMP_DISCOVERY_END count=%lu", mAmplifierCount);
#if defined(KERNEL)
        publishBootCheckpoint(this, "AMP_DISCOVERY_END", mAmplifierCount == expectedAmplifierCount() ? kIOReturnSuccess : kIOReturnNotFound);
#endif
    }

    if (bootArgEnabled("-cirrusro")) {
        CIRRUS_LOG("CirrusAudioFixup starting in READ-ONLY PROBE mode");
        uint32_t delayMs = 100;
        PE_parse_boot_argn("-cirrusdelay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
        publishDriverVerdict();
    } else {
        CIRRUS_LOG("CirrusAudioFixup starting FULL DRIVER FLOW");
        if (!mPlatformIdentityPending)
            bringUpAmplifiers();
        if (mProbeTimer) {
            mProbeTimer->setTimeoutMS(50);
        }
    }

    CIRRUS_LOG("BOOT_FLOW PM_SETUP_BEGIN");
#if defined(KERNEL)
    publishBootCheckpoint(this, "PM_SETUP_BEGIN");
#endif
    const bool powerManagementReady = mPMInitialized || setupPowerManagement(provider);
    CIRRUS_LOG("BOOT_FLOW PM_SETUP_END ready=%u", unsigned(powerManagementReady));
#if defined(KERNEL)
    publishBootCheckpoint(this, "PM_SETUP_END", powerManagementReady ? kIOReturnSuccess : kIOReturnNotReady);
    if (!powerManagementReady)
        retainFailedServiceEvidence(this);
#endif
    if (!powerManagementReady) {
        return false;
    }
    setupAmplifierInterrupt();
    registerService();
    CIRRUS_LOG("BOOT_FLOW START_END");
#if defined(KERNEL)
    publishBootCheckpoint(this, "START_END");
#endif
    return true;
}

void CirrusAudioFixup::stop(IOService* provider) {
    if (mCommandGate) {
        mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(uintptr_t(2)));
    } else {
        handlePowerChange(false);
        mStopping = true;
    }
    // Stop callbacks under the gate before releasing their event sources.
    teardownAmplifierInterrupt();
    teardownAudioEvents();
    if (mPMInitialized) {
        PMstop();
        mPMInitialized = false;
    }
    if (mProbeTimer && mWorkLoop) {
        mProbeTimer->cancelTimeout();
        mWorkLoop->removeEventSource(mProbeTimer);
    }
    OSSafeReleaseNULL(mProbeTimer);
    if (mCommandGate && mWorkLoop)
        mWorkLoop->removeEventSource(mCommandGate);
    OSSafeReleaseNULL(mCommandGate);
    OSSafeReleaseNULL(mWorkLoop);
    teardownCommunication();
    mProvider = nullptr;
    super::stop(provider);
}

void CirrusAudioFixup::free() {
    CIRRUS_LOG("free");
    teardownCommunication();
    clearHdaCache();
    if (mTraceLock) {
        IOLockFree(mTraceLock);
        mTraceLock = nullptr;
    }
    super::free();
}

bool CirrusAudioFixup::bootArgEnabled(const char* name) {
    char val[16] = {};
    if (PE_parse_boot_argn(name, val, sizeof(val))) {
        if (val[0] == '0' && val[1] == '\0') {
            return false;
        }
        return true;
    }
    return false;
}

bool CirrusAudioFixup::bootArgStrEquals(const char* name, const char* expectedVal) {
    char val[64] = {};
    if (PE_parse_boot_argn(name, val, sizeof(val))) {
        val[sizeof(val) - 1] = 0;
        return strncmp(val, expectedVal, sizeof(val)) == 0;
    }
    return false;
}
