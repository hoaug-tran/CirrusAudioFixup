//
// CirrusAudioFixup.cpp
// IOKit service for board-specific CS35L41 bring-up and playback control.
//
// Hardware readiness is established by register and DSP checks. HDA stream
// observation cannot confirm the downstream audio route or audible output.
// See LICENSE for distribution terms.
//

#include "CirrusAudioFixup.hpp"

#include "Devices/CS35L41/Resources/Firmware.hpp"
#include "Firmware/WMFW/FirmwareUploader.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

#define super IOService
using namespace cirrus::diagnostics;
OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)

    class CirrusAudioFixup;

class FixupRegisterIOAdapter : public cirrus::core::RegisterIO {
private:
    CirrusAudioFixup* mFixup;
    AmplifierState& mAmp;
    cirrus::diagnostics::TraceSource mSource;

public:
    FixupRegisterIOAdapter(CirrusAudioFixup* fixup, AmplifierState& amp,
                           cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_PROBE)
        : mFixup(fixup), mAmp(amp), mSource(source) {}

    bool read(uint32_t address, uint32_t* value) override {
        return mFixup->readRegister(mAmp, address, value, mSource);
    }

    bool write(uint32_t address, uint32_t value) override {
        return mFixup->writeRegister(mAmp, address, value, mSource);
    }

    bool updateBits(uint32_t address, uint32_t mask, uint32_t value) override {
        return mFixup->updateRegisterBits(mAmp, address, mask, value, mSource);
    }

    bool pollBit(uint32_t address, uint32_t mask, uint32_t expected, uint32_t timeoutMs) override {
        return mFixup->pollRegisterBit(mAmp, address, mask, expected, timeoutMs, cirrus::diagnostics::TRACE_PROBE);
    }

    bool bulkRead(uint32_t address, uint8_t* data, size_t length) override {
        return mFixup->bulkRead(mAmp, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }

    bool bulkWrite(uint32_t address, const uint8_t* data, size_t length) override {
        return mFixup->bulkWrite(mAmp, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }
};

static UInt32 readBE32(const UInt8* data) {
    return (static_cast<UInt32>(data[0]) << 24) | (static_cast<UInt32>(data[1]) << 16) | (static_cast<UInt32>(data[2]) << 8) |
           static_cast<UInt32>(data[3]);
}

static uint64_t playbackTimeMilliseconds() {
    uint64_t nanos = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
    return nanos / 1000000;
}

static void writeBE32(UInt8* data, UInt32 value) {
    data[0] = static_cast<UInt8>((value >> 24) & 0xFF);
    data[1] = static_cast<UInt8>((value >> 16) & 0xFF);
    data[2] = static_cast<UInt8>((value >> 8) & 0xFF);
    data[3] = static_cast<UInt8>(value & 0xFF);
}

bool gCirrusDebug = false;

bool CirrusAudioFixup::init(OSDictionary* properties) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = cirrus::support::kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg");
    if (!super::init(properties)) {
        CIRRUS_ERR("super::init failed");
        return false;
    }

    mTraceLock = IOLockAlloc();
    if (!mTraceLock)
        return false;
    initTraceBuffer();

    setProperty("CirrusReachedInit", kOSBooleanTrue);
    CIRRUS_LOG("init");
    return true;
}

IOService* CirrusAudioFixup::probe(IOService* provider, SInt32* score) {
    IOService* result = super::probe(provider, score);

    setProperty("CirrusReachedProbe", kOSBooleanTrue);

    CIRRUS_LOG("probe provider=%s class=%s score=%d", provider ? provider->getName() : "null",
               provider ? provider->getMetaClass()->getClassName() : "null", score ? static_cast<int>(*score) : -1);

    if (score) {
        *score += 9000;
    }

    return result;
}

// Read-only and discovery-only boots must leave the board GPIO untouched.
// This quirk writes a board-specific MMIO register. Resolve the profile first;
// finding an AMD controller alone is not permission to use its reset pin.
bool CirrusAudioFixup::performPlatformHardwareReset() {
    if (bootArgEnabled("-cirrusro") || bootArgStrEquals("-cirrusphase", "probe"))
        return true;
    const cirrus::platform::ResetControllerQuirk* quirkPtr = mPlatformProfile ? mPlatformProfile->resetQuirk : nullptr;
    if (quirkPtr) {
        const cirrus::platform::ResetControllerQuirk& quirk = *quirkPtr;
        OSDictionary* dict = IOService::nameMatching(quirk.controllerName);
        if (!dict)
            return true;

        OSIterator* iter = IOService::getMatchingServices(dict);
        dict->release();
        if (!iter)
            return true;

        IOService* controller = OSDynamicCast(IOService, iter->getNextObject());
        if (!controller) {
            iter->release();
            return true;
        }

        bool verified = false;
        if (controller->open(this)) {
            IOMemoryDescriptor* bmd = controller->getDeviceMemoryWithIndex(0);
            if (bmd && bmd->getLength() >= quirk.minMemoryLength) {
                if (bmd->prepare() == kIOReturnSuccess) {
                    IOMemoryMap* map = bmd->map();
                    if (map) {
                        volatile UInt32* base = (volatile UInt32*)map->getVirtualAddress();
                        if (base) {
                            UInt32 value = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_Old", value, 32);
                            value &= ~(1U << quirk.assertClearBit);
                            value |= (1U << quirk.outputEnableBit);
                            base[quirk.registerIndex] = value;
                            UInt32 verifyLow = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_VerifyLow", verifyLow, 32);

                            IOSleep(quirk.settleMs);

                            value = base[quirk.registerIndex];
                            value |= (1U << quirk.assertClearBit);
                            value |= (1U << quirk.outputEnableBit);
                            base[quirk.registerIndex] = value;
                            UInt32 verifyHigh = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_VerifyHigh", verifyHigh, 32);

                            verified =
                                ((verifyLow & (1U << quirk.assertClearBit)) == 0) && ((verifyHigh & (1U << quirk.assertClearBit)) != 0);
                            CIRRUS_LOG("platform hardware reset quirk %s %s", quirk.controllerName, verified ? "verified" : "not verified");
                        }
                        map->release();
                    }
                    bmd->complete();
                }
            }
            controller->close(this);
        }
        iter->release();
        return verified;
    }

    CIRRUS_LOG("No platform hardware reset quirk configured for profile %s; continuing with ACPI and I2C reset",
               mPlatformProfile ? mPlatformProfile->name : "unknown");
    return true;
}

void CirrusAudioFixup::resolvePlatformProfile() {
    const char* acpiHid = nullptr;
    if (mProvider) {
        OSString* ioName = OSDynamicCast(OSString, mProvider->getProperty("IOName"));
        if (ioName)
            acpiHid = ioName->getCStringNoCopy();
        if (!acpiHid)
            acpiHid = mProvider->getName();
    }

    mPlatformProfile = cirrus::platform::findPlatformProfile(acpiHid);
    if (!mPlatformProfile) {
        mPlatformProfile = &cirrus::platform::defaultPlatformProfile();
        CIRRUS_LOG("No exact platform profile for ACPI HID %s; using conservative profile %s", acpiHid ? acpiHid : "unknown",
                   mPlatformProfile->name);
    }

    setProperty("Cirrus_Platform_Profile", mPlatformProfile->name);
    setProperty("Cirrus_Platform_ACPI_HID", acpiHid ? acpiHid : "unknown");
    CIRRUS_LOG("platform profile selected: %s (hid=%s, endpoints=%lu)", mPlatformProfile->name, acpiHid ? acpiHid : "unknown",
               mPlatformProfile->endpointCount);
}

// Validate diagnostic options before any reset or amplifier write.
// Provider discovery happens before service publication; later playback and
// power transitions run on the service workloop.
bool CirrusAudioFixup::start(IOService* provider) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = cirrus::support::kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg");

    IOLog("============================================================\n");
    IOLog("  CirrusAudioFixup by Tran Kinh Hoang (hoaug-tran)\n");
    IOLog("  Smart Amplifier Fixup for macOS\n");
    IOLog("  If you paid for this, you have been scammed!\n");
    IOLog("============================================================\n");

    setProperty("Author", "Tran Kinh Hoang (hoaugtr)");
    setProperty("Cirrus_Build_Configuration", cirrus::support::kBuildConfiguration);
    setProperty("Cirrus_Audio_Event_Mode", "HDA_TIMER_FALLBACK");
    IOLog("CirrusAudioFixup: %s build; verbose logging %s\n", cirrus::support::kBuildConfiguration,
          gCirrusDebug ? "enabled" : "disabled");
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("start");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
        return false;
    }

    mProvider = provider;
    logProviderInfo(provider);
    dumpProviderProperties(provider);
    resolvePlatformProfile();

    char requestedPhase[32]{};
    if (PE_parse_boot_argn("-cirrusphase", requestedPhase, sizeof(requestedPhase))) {
        bool valid = false;
        static const char* phases[] = {"probe", "otp", "errata", "clock", "asp", "gpio", "platform", "firmware", "dsp"};
        for (const char* phase : phases) {
            if (strcmp(requestedPhase, phase) == 0)
                valid = true;
        }
        if (!valid) {
            CIRRUS_ERR("Unsupported -cirrusphase=%s; initialization rejected", requestedPhase);
            mProvider = nullptr;
            super::stop(provider);
            return false;
        }
    }

    bool hardwareResetVerified = performPlatformHardwareReset();
    if (!hardwareResetVerified) {
        CIRRUS_LOG("Platform hardware reset not asserted or verified; continuing with ACPI and I2C soft reset");
    }

    IOSleep(15);
    detectAmplifiers();

    if (!setupProbeTimer()) {
        CIRRUS_ERR("probe timer setup failed");
        mProvider = nullptr;
        super::stop(provider);
        return false;
    }

    bool probeEnabled = bootArgEnabled("-cirrusprobe");
    setProperty("CirrusBootArgParsed", probeEnabled ? kOSBooleanTrue : kOSBooleanFalse);

    if (bootArgEnabled("-cirrusro")) {
        CIRRUS_LOG("CirrusAudioFixup starting in READ-ONLY PROBE mode");
        uint32_t delayMs = 100;
        PE_parse_boot_argn("-cirrusdelay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
        publishDriverVerdict();
    } else {
        CIRRUS_LOG("CirrusAudioFixup starting FULL DRIVER FLOW");
        fullDriverFlow();
        if (mProbeTimer) {
            mProbeTimer->setTimeoutMS(50);
        }
    }

    if (!setupPowerManagement(provider)) {
        stop(provider);
        return false;
    }
    registerService();
    return true;
}

// Initialize each supported amplifier into verified, muted idle.
// Failed CS35L41 stages attempt output cleanup and a DSP halt before returning.
// Unsupported models are rejected without silicon-specific writes.
// Keep the original failure in diagnostics even if cleanup also fails.
// A debug phase stops at its boundary and never publishes playback readiness.
void CirrusAudioFixup::fullDriverFlow() {
    CIRRUS_LOG("starting hardware initialization flow");
    if (bootArgEnabled("-cirrusro"))
        return;

    for (size_t i = 0; i < mAmpCount; ++i) {
        AmplifierState& amp = mAmps[i];
        if (amp.playbackFaulted)
            continue;

        // Dispatch before any silicon-specific reset, register or firmware work.
        // New registry entries need their own lifecycle case here.
        switch (amp.model) {
        case cirrus::core::CodecModel::CS35L41:
            initializeCS35L41(amp);
            break;
        default:
            amp.initialized = amp.firmwareValidated = amp.dspAlive = false;
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_DEVICE_ID);
            break;
        }
    }

    publishDriverVerdict();
    CIRRUS_LOG("hardware initialization flow complete");
}

// CS35L41 lifecycle implementation. Do not reuse this sequence for another
// model: reset, OTP, DSP memory, mailbox and boost controls are silicon-specific.
// Unsupported channel layouts stop before the first amplifier write.
void CirrusAudioFixup::initializeCS35L41(AmplifierState& amp) {
    const bool bypass = bootArgEnabled("-cirrusnodsp");
    if (amp.channel != cirrus::core::AudioChannel::Left && amp.channel != cirrus::core::AudioChannel::Right) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID);
        return;
    }
    CIRRUS_LOG("initializing amplifier: %s", amp.name);
    if (stopAfterDebugStage("probe"))
        return;

    amp.initialized = amp.firmwareValidated = amp.dspAlive = false;
    amp.monitorCount = 0;
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;
    amp.firmwareIdVersion = 0;
    auto abortInitialization = [&]() {
        amp.playbackFaulted = true;
        amp.initialized = amp.firmwareValidated = false;

        if (amp.present) {
            if (!stopPlayback(amp))
                recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
            if (!stopDSP(amp))
                recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        }
        amp.dspAlive = false;
        amp.monitorCount = 0;
        amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
        amp.diagnosticControlCount = 0;
    };

    setDiagnosticStage(amp, STAGE_PROBE);

    if (!initCodec(amp)) {
        CIRRUS_ERR("failed to initialize codec for %s", amp.name);
        if (amp.diagnostic.latestFailure == DIAG_OK)
            recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                    cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_OTP_BOOT);
    if (stopAfterDebugStage("otp"))
        return;

    setDiagnosticStage(amp, STAGE_ERRATA);
    if (!initializeHardwareErrata(amp)) {
        CIRRUS_ERR("failed to apply hardware errata for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_ERRATA);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_ERRATA);
    if (stopAfterDebugStage("errata"))
        return;

    setDiagnosticStage(amp, STAGE_CLOCK);
    if (!applyPLL(amp)) {
        recordDiagnosticFailure(amp, DIAG_PLL_CONFIG, cirrus::devices::cs35l41::registers::kRegPllClockControl);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_CLOCK);
    if (stopAfterDebugStage("clock"))
        return;

    setDiagnosticStage(amp, STAGE_ASP);
    if (!applyASP(amp)) {
        recordDiagnosticFailure(amp, DIAG_ASP_CONFIG, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_ASP);
    if (stopAfterDebugStage("asp"))
        return;

    setDiagnosticStage(amp, STAGE_GPIO);
    if (!applyGPIO(amp)) {
        recordDiagnosticFailure(amp, DIAG_GPIO_CONFIG, cirrus::devices::cs35l41::registers::kRegGpioPadControl, 0x02000000);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_GPIO);
    if (stopAfterDebugStage("gpio"))
        return;

    setDiagnosticStage(amp, STAGE_PLATFORM);
    if (!configureHardware(amp)) {
        CIRRUS_ERR("platform hardware configuration failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_PLATFORM_CONFIG);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_PLATFORM);
    if (stopAfterDebugStage("platform"))
        return;

    if (bypass) {
        if (!stopDSP(amp)) {
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            abortInitialization();
            return;
        }
    } else {
        setDiagnosticStage(amp, STAGE_FIRMWARE_DISCOVERY);
        discoverFirmware(amp);
        if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
            recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
            abortInitialization();
            return;
        }
        markDiagnosticSuccess(amp, STAGE_FIRMWARE_DISCOVERY);
        setDiagnosticStage(amp, STAGE_FIRMWARE_UPLOAD);
        initializeFirmware(amp, "5D.0");
        if (bootArgStrEquals("-cirrusphase", "firmware") && mDebugPhaseHalted &&
            amp.firmwareValidated && !amp.playbackFaulted)
            return;
        if (amp.playbackFaulted || !amp.firmwareValidated || !amp.dspAlive || amp.monitorCount != 1) {
            if (amp.diagnostic.latestFailure == DIAG_OK)
                recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            abortInitialization();
            return;
        }
        markDiagnosticSuccess(amp, STAGE_DSP_BOOT);
    }

    if ((bypass && stopAfterDebugStage("firmware")) || stopAfterDebugStage("dsp")) {
        // Firmware initialization includes DSP boot; explicitly halt it before returning.
        if (!stopDSP(amp)) {
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        }
        return;
    }

    setDiagnosticStage(amp, STAGE_IDLE_VERIFY);
    if (!powerUpAmplifier(amp)) {
        recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
        abortInitialization();
        return;
    }
    if (!verifyIdleConfiguration(amp)) {
        CIRRUS_ERR("idle hardware verification failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_IDLE_INVARIANT);
        abortInitialization();
        return;
    }
    if (!bypass && !verifyDSPAlive(amp)) {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        abortInitialization();
        return;
    }
    markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
    logASPSnapshot(amp);

    amp.initialized = true;

    CIRRUS_LOG("amplifier %s initialized successfully (dspAlive=%d)", amp.name, amp.dspAlive);
}

// A phase stop belongs to the whole service, not just one amplifier.
// The monitor stays disabled after either channel reaches the requested stage;
// initialization still visits the other channel so its failures are visible.
bool CirrusAudioFixup::stopAfterDebugStage(const char* stage) {
    if (!bootArgStrEquals("-cirrusphase", stage))
        return false;
    mDebugPhaseHalted = true;
    setProperty("Cirrus_Debug_Phase", stage);
    return true;
}

bool CirrusAudioFixup::setupPowerManagement(IOService* provider) {
    PMinit();
    mPMInitialized = true;
    provider->joinPMtree(this);
    mPowerStates[0].version = 1;
    mPowerStates[1].version = 1;
    mPowerStates[1].capabilityFlags = kIOPMPowerOn | kIOPMDeviceUsable;
    mPowerStates[1].outputPowerCharacter = kIOPMPowerOn;
    mPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
    return registerPowerDriver(this, mPowerStates, 2) == kIOReturnSuccess;
}

// The command gate shares the timer's workloop serialization.
// Operation 0 suspends, 1 resumes and 2 stops the service. Set the stopping
// flag after suspend cleanup so that cleanup can still access the device.
IOReturn CirrusAudioFixup::lifecycleAction(OSObject* owner, void* operation, void*, void*, void*) {
    auto* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (!self)
        return kIOReturnBadArgument;
    const uintptr_t action = reinterpret_cast<uintptr_t>(operation);
    if (action == 2) {
        self->handlePowerChange(false);
        self->mStopping = true;
    } else if (!self->mStopping) {
        self->handlePowerChange(action != 0);
    }
    return kIOReturnSuccess;
}

IOReturn CirrusAudioFixup::setPowerState(unsigned long state, IOService* device) {
    if (device != this || state > 1)
        return IOPMAckImplied;
    if (mCommandGate) {
        IOReturn result = mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(state));
        if (result != kIOReturnSuccess)
            CIRRUS_ERR("PM gate failed: 0x%08X", result);
    }
    return IOPMAckImplied;
}

// Quiesce output before releasing the HDA mapping on suspend.
// Wake repeats the amplifier initialization flow, including its software reset;
// it does not repeat the board GPIO reset performed during start().
void CirrusAudioFixup::handlePowerChange(bool powered) {
    if (mStopping || powered == mPowerAvailable)
        return;
    if (!powered) {
        if (mProbeTimer)
            mProbeTimer->cancelTimeout();

        if (!bootArgEnabled("-cirrusro")) {
            for (auto& amp : mAmps) {
                if (!amp.present)
                    continue;
                bool idle = stopPlayback(amp);
                bool halted = stopDSP(amp);
                if (!idle || !halted)
                    amp.playbackFaulted = true;
                amp.initialized = false;
                amp.firmwareValidated = false;
                amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
            }
        }
        mPowerAvailable = false;
        clearHdaCache();
        mHdaState.streamActive = mHdaState.observed = mHdaState.converterPrepared = false;
        mHdaState.lastDescriptor = 0xFF;
        mHdaState.lastStreamTag = 0;
        mHdaState.lastFormat = 0;
        setProperty("Cirrus_PM_Powered", uint64_t(0), 32);
    } else {
        mPowerAvailable = true;
        mNeedsReinitialization = !bootArgEnabled("-cirrusro");
        mHdaState.topologyLogged = false;
        setProperty("Cirrus_PM_Powered", uint64_t(1), 32);

        if (mNeedsReinitialization) {
            fullDriverFlow();
            mNeedsReinitialization = false;
        }
        if (mProbeTimer)
            mProbeTimer->setTimeoutMS(50);
    }
    publishDriverVerdict();
}

// Finish hardware cleanup while the gate and workloop still exist.
// Cancel and remove event sources before releasing their owners; a timer
// callback must never outlive the provider pointer it uses.
void CirrusAudioFixup::stop(IOService* provider) {
    teardownAudioEvents();
    if (mCommandGate) {
        mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(uintptr_t(2)));
    } else {
        handlePowerChange(false);
        mStopping = true;
    }
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
    mProvider = nullptr;
    super::stop(provider);
}

void CirrusAudioFixup::free() {
    CIRRUS_LOG("free");
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
        return strncmp(val, expectedVal, sizeof(val)) == 0;
    }
    return false;
}

void CirrusAudioFixup::logProviderInfo(IOService* provider) {
    if (!provider) {
        CIRRUS_ERR("provider is null");
        return;
    }

    CIRRUS_LOG("provider name=%s class=%s", provider->getName(), provider->getMetaClass()->getClassName());

    OSObject* addrObj = provider->getProperty("i2cAddress");
    if (OSNumber* addr = OSDynamicCast(OSNumber, addrObj)) {
        CIRRUS_LOG("provider i2cAddress=0x%02X", addr->unsigned32BitValue());
    }

    OSObject* modeObj = provider->getProperty("Interrupt Mode");
    if (OSString* mode = OSDynamicCast(OSString, modeObj)) {
        CIRRUS_LOG("provider interrupt=%s", mode->getCStringNoCopy());
    }

    OSObject* pathObj = provider->getProperty("acpi-path");
    if (OSString* path = OSDynamicCast(OSString, pathObj)) {
        CIRRUS_LOG("provider acpi-path=%s", path->getCStringNoCopy());
    }
}

void CirrusAudioFixup::dumpProviderProperties(IOService* provider) {
    if (!provider) {
        return;
    }

    CIRRUS_LOG("provider properties follow");

    OSDictionary* properties = provider->getPropertyTable();
    OSCollectionIterator* iterator = OSCollectionIterator::withCollection(properties);
    if (!iterator) {
        CIRRUS_ERR("property iterator failed");
        return;
    }

    while (OSObject* key = iterator->getNextObject()) {
        OSString* keyString = OSDynamicCast(OSString, key);
        if (!keyString) {
            continue;
        }

        OSObject* value = properties->getObject(keyString);
        if (!value) {
            continue;
        }

        if (OSString* str = OSDynamicCast(OSString, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), str->getCStringNoCopy());
        } else if (OSNumber* num = OSDynamicCast(OSNumber, value)) {
            CIRRUS_LOG("property %s=0x%llX", keyString->getCStringNoCopy(), num->unsigned64BitValue());
        } else if (OSBoolean* boo = OSDynamicCast(OSBoolean, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), boo->getValue() ? "true" : "false");
        } else {
            CIRRUS_LOG("property %s class=%s", keyString->getCStringNoCopy(), value->getMetaClass()->getClassName());
        }
    }

    iterator->release();
}

bool CirrusAudioFixup::setupProbeTimer() {
    mWorkLoop = getWorkLoop();
    if (!mWorkLoop) {
        mWorkLoop = IOWorkLoop::workLoop();
    } else {
        mWorkLoop->retain();
    }

    if (!mWorkLoop) {
        return false;
    }

    mProbeTimer = IOTimerEventSource::timerEventSource(this, probeTimerFired);
    if (!mProbeTimer) {
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    if (mWorkLoop->addEventSource(mProbeTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    mCommandGate = IOCommandGate::commandGate(this);
    if (!mCommandGate || mWorkLoop->addEventSource(mCommandGate) != kIOReturnSuccess) {
        OSSafeReleaseNULL(mCommandGate);
        mWorkLoop->removeEventSource(mProbeTimer);
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }
    setProperty("CirrusTimerCreated", kOSBooleanTrue);
    return true;
}

// Upload and verify firmware and channel tuning before starting the DSP.
// The parsed image is temporary and must be freed on every exit, including
// a requested firmware-phase halt. Readiness requires the later boot checks.
void CirrusAudioFixup::initializeFirmware(AmplifierState& amp, const char* phaseArg) {
    (void)phaseArg;
    CIRRUS_LOG("starting full initialization for amplifier: %s", amp.name);
    amp.firmwareValidated = false;
    amp.dspAlive = false;
    amp.monitorCount = 0;

    if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
        CIRRUS_ERR("firmware or tuning data missing for %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
        return;
    }

    bool wmfwUploaded = false;
    bool coefficientsUploaded = false;

    if (!stopDSP(amp)) {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.playbackFaulted = true;
        return;
    }
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;

    FirmwareImage* image = (FirmwareImage*)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD, 0, (UInt32)sizeof(FirmwareImage), 0, kIOReturnNoMemory, false);
        return;
    }

    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("wmfw parsing successful on %s. fw id: 0x%06X, expected algs: %u", amp.name, image->firmwareId, image->algorithmTotal);

    MappedImage* wmfwMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
    if (wmfwMapped) {
        if (CirrusFirmwareMapper::mapFirmwareImage(*image, *wmfwMapped)) {
            CIRRUS_LOG("mapping wmfw image successful on %s, starting upload", amp.name);
            UploadSession session;
            FixupRegisterIOAdapter io(this, amp);
            wmfwUploaded = CirrusFirmwareScheduler::run(amp.name, io, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("failed to map wmfw image on %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    if (!wmfwUploaded) {
        CIRRUS_ERR("wmfw upload failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("parsing dsp algorithms for %s", amp.name);
    if (!parseDSPAlgorithms(amp, *image)) {
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    WMFWControlRef stateRef{}, heartbeatRef{};
    if (!CirrusFirmwareParser::findControl(image, "HALO_STATE", stateRef) ||
        !CirrusFirmwareParser::findControl(image, "HALO_HEARTBEAT", heartbeatRef) ||
        (stateRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) ||
        (heartbeatRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) || stateRef.control->type != WMFW_ADSP2_XM ||
        stateRef.control->len != 4 || heartbeatRef.control->type != WMFW_ADSP2_XM || heartbeatRef.control->len != 4 ||
        !CirrusFirmwareParser::resolveControl(*image, stateRef, amp.haloStateRegister) ||
        !CirrusFirmwareParser::resolveControl(*image, heartbeatRef, amp.haloHeartbeatRegister)) {
        amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    CIRRUS_LOG("HALO controls on %s: state=0x%08X heartbeat=0x%08X firmware=0x%06X", amp.name, amp.haloStateRegister,
               amp.haloHeartbeatRegister, image->firmwareId);
    amp.firmwareIdVersion = image->haloFirmwareVersion;
    CIRRUS_LOG("HALO firmware ID version on %s: 0x%06X", amp.name, amp.firmwareIdVersion);
    char fwIdVersionProp[64];
    snprintf(fwIdVersionProp, sizeof(fwIdVersionProp), "Cirrus_FW_ID_Version_%s", amp.name);
    setProperty(fwIdVersionProp, (uint64_t)amp.firmwareIdVersion, 32);

    auto containsStr = [](const char* str, const char* sub) -> bool {
        if (!str || !sub)
            return false;
        size_t strLen = strlen(str);
        size_t subLen = strlen(sub);
        if (subLen > strLen)
            return false;
        for (size_t i = 0; i <= strLen - subLen; i++) {
            bool match = true;
            for (size_t j = 0; j < subLen; j++) {
                char c1 = str[i + j];
                char c2 = sub[j];
                if (c1 >= 'a' && c1 <= 'z')
                    c1 -= 32;
                if (c2 >= 'a' && c2 <= 'z')
                    c2 -= 32;
                if (c1 != c2) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    };

    amp.diagnosticControlCount = 0;
    for (uint32_t i = 0; i < image->wmfwControlCount; i++) {
        const WMFWControl& ctl = image->wmfwControls[i];
        if (containsStr(ctl.name, "PCM") || containsStr(ctl.name, "LEVEL") || containsStr(ctl.name, "PEAK") ||
            containsStr(ctl.name, "RMS") || containsStr(ctl.name, "STREAM") || containsStr(ctl.name, "ACTIVE")) {
            if (amp.diagnosticControlCount < 10) {
                WMFWControlRef ref{};
                uint32_t regAddress = 0;
                if (ctl.len == 4 && (ctl.type == WMFW_ADSP2_XM || ctl.type == WMFW_ADSP2_YM) &&
                    CirrusFirmwareParser::findControl(image, ctl.name, ref) &&
                    CirrusFirmwareParser::resolveControl(*image, ref, regAddress)) {
                    strlcpy(amp.diagnosticControls[amp.diagnosticControlCount].name, ctl.name, sizeof(amp.diagnosticControls[0].name));
                    amp.diagnosticControls[amp.diagnosticControlCount].address = regAddress;
                    CIRRUS_LOG("registered diagnostic control '%s' at 0x%08X on %s", ctl.name, regAddress, amp.name);
                    amp.diagnosticControlCount++;
                }
            }
        }
    }
    const char* calibrationControls[] = {"CAL_AMBIENT", "CAL_R", "CAL_STATUS", "CAL_CHECKSUM"};
    for (const char* name : calibrationControls) {
        WMFWControlRef ref{};
        uint32_t address = 0;
        if (CirrusFirmwareParser::findControl(image, name, ref) && ref.algorithm->id == 0xCD && ref.control->type == WMFW_ADSP2_XM &&
            ref.control->len == 4 && CirrusFirmwareParser::resolveControl(*image, ref, address) &&
            amp.diagnosticControlCount < sizeof(amp.diagnosticControls) / sizeof(amp.diagnosticControls[0])) {
            auto& control = amp.diagnosticControls[amp.diagnosticControlCount++];
            strlcpy(control.name, name, sizeof(control.name));
            control.address = address;
        }
    }
    char calibrationProperty[80];
    snprintf(calibrationProperty, sizeof(calibrationProperty), "Cirrus_Calibration_Status_%s", amp.name);
    OSString* calibrationStatus = OSString::withCString("NOT_APPLIED_BY_DRIVER");
    if (calibrationStatus) {
        setProperty(calibrationProperty, calibrationStatus);
        calibrationStatus->release();
    }
    CIRRUS_LOG("found %u matching diagnostic controls on %s", amp.diagnosticControlCount, amp.name);

    if (CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, image)) {
        CIRRUS_LOG("bin parsing successful on %s, found %u coefficient blocks", amp.name, image->coefficientCount);
        MappedImage* coeffMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
        if (coeffMapped) {
            if (CirrusFirmwareMapper::mapCoefficients(*image, *coeffMapped) && coeffMapped->regionCount > 0) {
                CIRRUS_LOG("starting coefficient file upload on %s", amp.name);
                UploadSession session;
                FixupRegisterIOAdapter io(this, amp);
                coefficientsUploaded = CirrusFirmwareScheduler::run(amp.name, io, *coeffMapped, session);
            } else {
                CIRRUS_ERR("coefficient mapping produced no uploadable regions on %s", amp.name);
                recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
            }
            IOFree(coeffMapped, sizeof(MappedImage));
        }
    } else {
        CIRRUS_ERR("bin file parsing failed on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
    }

    if (!coefficientsUploaded) {
        CIRRUS_ERR("coefficient upload failed on %s; keeping DSP stopped and output disabled", amp.name);
        if (amp.diagnostic.latestFailure != DIAG_COEFFICIENT_PARSE)
            recordDiagnosticFailure(amp, DIAG_COEFFICIENT_UPLOAD);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!applyCalibration(amp, image)) {
        CIRRUS_ERR("calibration failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_CALIBRATION);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    amp.firmwareValidated = true;

    CIRRUS_LOG("bringing up dsp controller on %s", amp.name);
    if (stopAfterDebugStage("firmware")) {
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    bool booted = bringupDSP(amp);

    if (booted && verifyDSPAlive(amp)) {
        CIRRUS_LOG("dsp is successfully verified alive on %s", amp.name);
        amp.dspAlive = true;
        amp.monitorCount = 1;
    } else {
        CIRRUS_ERR("dsp bringup failed or dsp is unresponsive on %s; disabling DSP mode", amp.name);
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.firmwareValidated = false;
        amp.dspAlive = false;
        amp.monitorCount = 0;
        stopDSP(amp);
    }

    logDSPBootReport(amp);

    IOFree(image, sizeof(FirmwareImage));
    CIRRUS_LOG("full firmware init complete for amplifier %s", amp.name);
}

struct cs35l41_amp_cal_data {
    uint32_t calTarget[2];
    uint32_t calTime[2];
    int8_t calAmbient;
    uint8_t calStatus;
    uint16_t calR;
} __attribute__((packed));

struct cs35l41_amp_efi_data {
    uint32_t size;
    uint32_t count;
    cs35l41_amp_cal_data data[];
} __attribute__((packed));

// Apply measured calibration only when the source and DSP controls are valid.
// EFI data is retained while parsing; the channel count must fit the property
// length. Missing calibration is reported, never fabricated.
// Remove old channel properties before a retry so they cannot look current.
bool CirrusAudioFixup::applyCalibration(AmplifierState& amp, const FirmwareImage* image) {
    const char* fields[] = {"Status", "R0", "Ambient", "Valid", "Checksum"};
    for (const char* field : fields) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_%s_%s", field, amp.name);
        removeProperty(property);
    }
    if (!image)
        return false;

    if (bootArgEnabled("-cirrusnocal")) {
        char calProp[80];
        snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* calVal = OSString::withCString("SKIPPED_BY_BOOT_ARG");
        if (calVal) {
            setProperty(calProp, calVal);
            calVal->release();
        }
        return true;
    }

    uint32_t addrAmbient = 0, addrR = 0, addrStatus = 0, addrChecksum = 0;
    WMFWControlRef refAmbient{}, refR{}, refStatus{}, refChecksum{};
    bool resolved =
        CirrusFirmwareParser::findControl(image, "CAL_AMBIENT", refAmbient) && refAmbient.algorithm->id == 0xCD &&
        refAmbient.control->type == WMFW_ADSP2_XM && refAmbient.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refAmbient, addrAmbient) && CirrusFirmwareParser::findControl(image, "CAL_R", refR) &&
        refR.algorithm->id == 0xCD && refR.control->type == WMFW_ADSP2_XM && refR.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refR, addrR) && CirrusFirmwareParser::findControl(image, "CAL_STATUS", refStatus) &&
        refStatus.algorithm->id == 0xCD && refStatus.control->type == WMFW_ADSP2_XM && refStatus.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refStatus, addrStatus) &&
        CirrusFirmwareParser::findControl(image, "CAL_CHECKSUM", refChecksum) && refChecksum.algorithm->id == 0xCD &&
        refChecksum.control->type == WMFW_ADSP2_XM && refChecksum.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refChecksum, addrChecksum);

    if (!resolved) {
        CIRRUS_ERR("failed to resolve all 4 calibration controls in algorithm 0xCD on %s", amp.name);
        char calProp[80];
        snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* calVal = OSString::withCString("CONTROL_RESOLUTION_FAILED");
        if (calVal) {
            setProperty(calProp, calVal);
            calVal->release();
        }
        return false;
    }

    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    bool foundData = false;
    bool invalidData = false;
    bool uidReadFailed = false;
    int32_t ambientVal = 0;
    uint32_t statusVal = 1;
    uint32_t r0Val = 0;
    uint32_t checksumVal = 0;
    const char* sourceStr = "NONE";
    auto publishStatus = [&](const char* status) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* value = OSString::withCString(status);
        if (value) {
            setProperty(property, value);
            value->release();
        }
    };

    IORegistryEntry* options = IORegistryEntry::fromPath("IODeviceTree:/options");
    if (!options) {
        options = IORegistryEntry::fromPath("IODeviceTree:/chosen");
    }
    if (options) {
        OSObject* prop = options->copyProperty("02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData");
        if (!prop) {
            prop = options->copyProperty("CirrusSmartAmpCalibrationData");
        }
        invalidData = prop != nullptr;
        OSData* calData = OSDynamicCast(OSData, prop);
        if (calData) {
            uint32_t len = calData->getLength();
            const uint8_t* rawBytes = (const uint8_t*)calData->getBytesNoCopy();
            if (rawBytes && len >= sizeof(cs35l41_amp_efi_data)) {
                const cs35l41_amp_efi_data* efiData = (const cs35l41_amp_efi_data*)rawBytes;
                uint32_t count = OSSwapLittleToHostInt32(efiData->count);
                if (count <= (len - sizeof(cs35l41_amp_efi_data)) / sizeof(cs35l41_amp_cal_data)) {
                    invalidData = false;
                    const cs35l41_amp_cal_data* selected = nullptr;
                    auto targetOf = [](const cs35l41_amp_cal_data& entry) -> uint64_t {
                        return ((uint64_t)OSSwapLittleToHostInt32(entry.calTarget[1]) << 32) |
                               OSSwapLittleToHostInt32(entry.calTarget[0]);
                    };
                    bool hasTarget = false;
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto& entry = efiData->data[i];
                        if ((entry.calTime[0] || entry.calTime[1]) && targetOf(entry))
                            hasTarget = true;
                    }

                    // Linux assembles the silicon UID from DIE_STS2 (high)
                    // and DIE_STS1 (low). Only targeted EFI records need these
                    // reads; a wildcard-only table can be selected by index.
                    uint64_t siliconUid = 0;
                    if (hasTarget) {
                        uint32_t high = 0, low = 0;
                        uidReadFailed = !readRegister(amp, 0x00017044, &high, TRACE_FIRMWARE) ||
                                        !readRegister(amp, 0x00017040, &low, TRACE_FIRMWARE);
                        siliconUid = ((uint64_t)high << 32) | low;
                        if (!uidReadFailed && siliconUid) {
                            for (uint32_t i = 0; i < count; ++i) {
                                const auto& entry = efiData->data[i];
                                if ((entry.calTime[0] || entry.calTime[1]) && targetOf(entry) == siliconUid) {
                                    selected = &entry;
                                    break;
                                }
                            }
                        }
                    }

                    // An unmatched nonzero target must not silently become
                    // another speaker's calibration. Linux permits index
                    // fallback only when the entry or silicon UID is zero.
                    if (!uidReadFailed && !selected && ampIdx < count) {
                        const auto& entry = efiData->data[ampIdx];
                        if ((entry.calTime[0] || entry.calTime[1]) && (!targetOf(entry) || !siliconUid))
                            selected = &entry;
                    }
                    if (selected) {
                        ambientVal = selected->calAmbient;
                        statusVal = selected->calStatus;
                        r0Val = OSSwapLittleToHostInt16(selected->calR);
                        checksumVal = r0Val + 1;
                        foundData = true;
                        sourceStr = "APPLIED_FROM_EFI_VARIABLE";
                    }
                }
            }
        }
        if (prop)
            prop->release();
        options->release();
    }
    if (uidReadFailed) {
        publishStatus("UID_READ_FAILED");
        return false;
    }
    if (invalidData) {
        publishStatus("INVALID_EFI_DATA");
        return false;
    }

    if (!foundData) {
        uint32_t bootR0 = 0;
        const char* r0Arg = (ampIdx == 0) ? "-cirruscalr0l" : "-cirruscalr0r";
        if (PE_parse_boot_argn(r0Arg, &bootR0, sizeof(bootR0)) || PE_parse_boot_argn("-cirruscalr0", &bootR0, sizeof(bootR0))) {
            r0Val = bootR0;
            statusVal = 1;
            ambientVal = 0;
            PE_parse_boot_argn("-cirruscalstatus", &statusVal, sizeof(statusVal));
            bool hasAmbient = PE_parse_boot_argn("-cirruscalambient", &ambientVal, sizeof(ambientVal));
            if (!hasAmbient || !r0Val || r0Val > 0xFFFFU || statusVal > 0xFFU || ambientVal < -128 || ambientVal > 127) {
                publishStatus("INVALID_BOOT_ARG_DATA");
                return false;
            }
            checksumVal = r0Val + 1;
            foundData = true;
            sourceStr = "APPLIED_FROM_BOOT_ARG";
        }
    }

    if (!foundData) {
        publishStatus("NOT_AVAILABLE");
        return true;
    }

    bool ok = writeRegister(amp, addrAmbient, (uint32_t)ambientVal, TRACE_FIRMWARE) && writeRegister(amp, addrR, r0Val, TRACE_FIRMWARE) &&
              writeRegister(amp, addrStatus, statusVal, TRACE_FIRMWARE) && writeRegister(amp, addrChecksum, checksumVal, TRACE_FIRMWARE);

    uint32_t rbAmbient = 0, rbR = 0, rbStatus = 0, rbChecksum = 0;
    bool verified = ok && readRegister(amp, addrAmbient, &rbAmbient, TRACE_FIRMWARE) && readRegister(amp, addrR, &rbR, TRACE_FIRMWARE) &&
                    readRegister(amp, addrStatus, &rbStatus, TRACE_FIRMWARE) &&
                    readRegister(amp, addrChecksum, &rbChecksum, TRACE_FIRMWARE) && rbAmbient == (uint32_t)ambientVal && rbR == r0Val &&
                    rbStatus == statusVal && rbChecksum == checksumVal;

    char calProp[80];
    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
    OSString* calVal = OSString::withCString(verified ? sourceStr : "VERIFY_FAILED");
    if (calVal) {
        setProperty(calProp, calVal);
        calVal->release();
    }

    if (!verified) {
        CIRRUS_ERR("calibration verification failed on %s: ok=%d verified=%d", amp.name, ok, verified);
        return false;
    }

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_R0_%s", amp.name);
    setProperty(calProp, (uint64_t)r0Val, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Ambient_%s", amp.name);
    setProperty(calProp, (uint64_t)ambientVal, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Valid_%s", amp.name);
    setProperty(calProp, (uint64_t)statusVal, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Checksum_%s", amp.name);
    setProperty(calProp, (uint64_t)checksumVal, 32);

    CIRRUS_LOG("calibration applied and verified on %s from %s: R0=%u, Ambient=%d, Status=%u, Checksum=%u", amp.name, sourceStr, r0Val,
               ambientVal, statusVal, checksumVal);
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
        if (self->mStopping || !self->mPowerAvailable)
            return;
        self->setProperty("CirrusTimerFired", kOSBooleanTrue);
        if (self->bootArgEnabled("-cirrusro")) {
            self->runReadOnlyProbe();
        } else {
            self->runBackgroundMonitor();
        }
    }
}

// Probe only profile endpoints permitted by board policy.
// An ACPI HID describes the provider, not the amplifier silicon. Check DEVID
// against the registry before using the endpoint's channel and tuning.
size_t CirrusAudioFixup::detectAmplifiers() {
    for (size_t i = 0; i < kMaxAmps; ++i) {
        mAmps[i].present = false;
    }

    const cirrus::platform::PlatformProfile& profile = mPlatformProfile ? *mPlatformProfile : cirrus::platform::defaultPlatformProfile();
    bool explicitLegacyProbe = bootArgEnabled("-cirruslegacyprobe");
    size_t candidateCount = profile.allowAutomaticInitialization || explicitLegacyProbe
                                ? (profile.endpointCount < kMaxAmps ? profile.endpointCount : kMaxAmps)
                                : 0;

    size_t detected = 0;
    mProbingAmplifiers = true;
    for (size_t i = 0; i < candidateCount; ++i) {
        const cirrus::platform::AmplifierEndpoint& endpoint = profile.endpoints[i];
        uint8_t addr = endpoint.address;
        uint8_t writeBuf[4] = {0x00, 0x00, 0x00, 0x00};
        uint8_t readBuf[4] = {0};

        bool ok = transferToAddress(addr, writeBuf, sizeof(writeBuf), readBuf, sizeof(readBuf));
        uint32_t devId = 0;
        if (ok) {
            devId = (static_cast<uint32_t>(readBuf[0]) << 24) | (static_cast<uint32_t>(readBuf[1]) << 16) |
                    (static_cast<uint32_t>(readBuf[2]) << 8) | static_cast<uint32_t>(readBuf[3]);
        }

        const cirrus::core::DeviceDescriptor* descriptor = cirrus::core::findDeviceById(devId);
        if (descriptor && descriptor->model == profile.amplifierModel) {
            mAmps[detected].name = endpoint.name;
            mAmps[detected].address = addr;
            mAmps[detected].model = descriptor->model;
            mAmps[detected].channel = endpoint.channel;
            mAmps[detected].present = true;
            mAmps[detected].deviceId = devId;
            detected++;
            CIRRUS_LOG("detected %s amplifier %s at address 0x%02X (DEVID=0x%08X)", descriptor->name, endpoint.name, addr, devId);
        } else if (ok && devId != 0) {
            CIRRUS_ERR("unsupported amplifier at address 0x%02X (DEVID=0x%08X, profile=%s)", addr, devId, profile.name);
        }
    }
    mProbingAmplifiers = false;

    if (detected == 0 && explicitLegacyProbe) {
        size_t fallbackCount = candidateCount < 2 ? candidateCount : 2;
        for (size_t i = 0; i < fallbackCount; ++i) {
            mAmps[i].name = profile.endpoints[i].name;
            mAmps[i].address = profile.endpoints[i].address;
            mAmps[i].model = profile.amplifierModel;
            mAmps[i].channel = profile.endpoints[i].channel;
            mAmps[i].present = true;
        }
        detected = fallbackCount;
        setProperty("Cirrus_Discovery_Fallback", kOSBooleanTrue);
        CIRRUS_LOG("legacy amplifier fallback explicitly enabled for %lu endpoint(s)", detected);
    } else {
        setProperty("Cirrus_Discovery_Fallback", kOSBooleanFalse);
    }

    mAmpCount = detected;
    setProperty("Cirrus_Detected_Amplifiers", (uint64_t)mAmpCount, 32);
    setProperty("Cirrus_Amp_Discovery", mAmpCount ? "DETECTED" : "NONE");
    CIRRUS_LOG("dynamic amplifier detection complete: %lu speaker(s) active (profile=%s)", mAmpCount, profile.name);
    return mAmpCount;
}

void CirrusAudioFixup::runReadOnlyProbe() {
    CIRRUS_LOG("read-only probe begin");

    for (size_t i = 0; i < mAmpCount; ++i) {
        probeAmp(mAmps[i]);
    }

    publishStatistics();

    if (bootArgEnabled("-cirrusdumptrace")) {
        dumpTraceBuffer();
    }

    CIRRUS_LOG("read-only probe complete");
}

bool CirrusAudioFixup::supportedHdaFormat(uint16_t format) {
    return cirrus::platform::hda::HDAStreamWatcher::isFormatSupported(format);
}

// The mapping and PCI service are owned references once cached.
// Call under lifecycle serialization, or from free() after event sources are
// gone. No callback may continue using the BAR after these releases.
void CirrusAudioFixup::clearHdaCache() {
    teardownAudioEvents();
    OSSafeReleaseNULL(mHdaMap);
    OSSafeReleaseNULL(mHdaPci);
    mHdaStatus = nullptr;
    mHdaState.topologyLogged = false;
}

// Observe HDA without changing its DMA registers.
// Keep the PCI service and BAR mapping across polls. Drop them on suspend,
// removal or invalid topology; unavailable hardware must disable playback.
// Multiple running output streams are ambiguous, so none is selected.
bool CirrusAudioFixup::synchronizeHdaStream() {
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
    IOService* audioCtrl = mHdaPci ? mHdaPci : getAudioController();
    if (!audioCtrl) {
        if (mHdaState.missCount < 20 && ++mHdaState.missCount == 20) {
            for (size_t i = 0; i < mAmpCount; ++i) {
                setDiagnosticStage(mAmps[i], STAGE_HDA_DETECT);
                recordDiagnosticFailure(mAmps[i], DIAG_HDA_CONTROLLER, 0, 1, 0, kIOReturnNotFound, false);
            }
        }
        publishStatus(mHdaState.missCount >= 20 ? "MISSING_AFTER_20_POLLS" : "MISSING");
        return false;
    }
    if (mHdaState.missCount != 0) {
        CIRRUS_LOG("HDA controller discovered after %u missed polls", mHdaState.missCount);
        mHdaState.missCount = 0;
    }

    IOPCIDevice* pciDev = OSDynamicCast(IOPCIDevice, audioCtrl);
    if (!pciDev) {
        publishStatus("NOT_A_PCI_CONTROLLER");
        audioCtrl->release();
        return false;
    }
    if (pciDev->isInactive() || pciDev->configRead16(kIOPCIConfigVendorID) == 0xFFFF) {
        publishStatus("CONTROLLER_UNAVAILABLE");
        if (mHdaPci)
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

    if (outputRunning && activeStream != 0 && !supportedHdaFormat(activeFormat)) {
        for (size_t i = 0; i < mAmpCount; ++i) {
            setDiagnosticStage(mAmps[i], STAGE_HDA_DETECT);
            recordDiagnosticFailure(mAmps[i], DIAG_HDA_STREAM_FORMAT, activeDescriptor, 1, activeFormat, kIOReturnSuccess, false);
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

// Protection faults are evidence, not events to clear and retry blindly.
// A failed status read is also unsafe: the caller must leave output disabled.
bool CirrusAudioFixup::checkProtectionStatus(AmplifierState& amp) {
    uint32_t status = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kIrq1Status1Register, &status, TRACE_PLAYBACK))
        return false;
    char prop[80];
    snprintf(prop, sizeof(prop), "Cirrus_Protection_Fault_%s", amp.name);
    if (status & cirrus::devices::cs35l41::registers::kMaskProtectionFault) {
        const char* faultReason = "UNKNOWN_FAULT";
        if (status & 0x00000080) {
            faultReason = "CRITICAL_OVERTEMPERATURE";
        } else if (status & 0x80000000) {
            faultReason = "SPEAKER_SHORT_CIRCUIT";
        } else if (status & 0x00020000) {
            faultReason = "BOOST_OVERVOLTAGE";
        } else if (status & 0x00008000) {
            faultReason = "BOOST_SHORT_CIRCUIT";
        } else if (status & 0x00000040) {
            faultReason = "BOOST_PEAK_CURRENT_LIMIT";
        } else if (status & 0x00000100) {
            faultReason = "TEMPERATURE_WARNING";
        }
        OSString* faultVal = OSString::withCString(faultReason);
        if (faultVal) {
            setProperty(prop, faultVal);
            faultVal->release();
        }
        recordDiagnosticFailure(amp, DIAG_AMP_PROTECTION, cirrus::devices::cs35l41::registers::kIrq1Status1Register, 0, status);
        return false;
    }
    OSString* okVal = OSString::withCString("OK");
    if (okVal) {
        setProperty(prop, okVal);
        okVal->release();
    }
    return true;
}

// Mute, remove global power and verify the resulting idle registers.
// Continue best-effort cleanup after individual writes fail. Report idle only
// when the final readback and power-down acknowledgement agree.
// playbackActive remains true after unverified cleanup to allow bounded retries.
bool CirrusAudioFixup::stopPlayback(AmplifierState& amp) {
    if (amp.model != cirrus::core::CodecModel::CS35L41) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
        return false;
    }
    // Cancel preparation before cleanup so no later callback can commit it.
    amp.playbackTransition = {};
    amp.shutdownTransition = {};
    amp.clockWaitAfterCleanup = false;
    ++amp.cleanupAttempts;
    setDiagnosticStage(amp, STAGE_PLAYBACK_CLEANUP);
    amp.playbackStableCount = 0;
    bool ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0, TRACE_PLAYBACK);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678, TRACE_PLAYBACK) && ok;
    uint32_t before = 0;
    bool known = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &before, TRACE_PLAYBACK);
    bool needPdn = !known || (before & 1);
    ok = known && ok;
    bool unlocked = unlockTestKey(amp);
    ok = unlocked && ok;
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00585941, TRACE_PLAYBACK) && ok;
    if (needPdn)
        ok = writeRegister(amp, 0x00010010, 0x00800000, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl1, 1, 0, TRACE_PLAYBACK) && ok;
    if (unlocked)
        ok = writeRegister(amp, 0x0000742C, 0x00000009, TRACE_PLAYBACK) && ok;
    bool pdnDone = !needPdn;
    uint32_t irq = 0;
    for (unsigned i = 0; needPdn && i < 100; ++i) {
        if (!readRegister(amp, 0x00010010, &irq, TRACE_PLAYBACK))
            break;
        if (irq & 0x00800000) {
            pdnDone = true;
            ok = writeRegister(amp, 0x00010010, 0x00800000, TRACE_PLAYBACK) && ok;
            break;
        }
        IOSleep(1);
    }
    if (!pdnDone)
        recordDiagnosticFailure(amp, DIAG_POWER_DOWN_TIMEOUT, 0x00010010, 0x00800000, irq);
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00580941, TRACE_PLAYBACK) && ok;
    ok = lockTestKey(amp) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegGpio1Control1, 0x00000001, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 1, 0, TRACE_PLAYBACK) && ok;
    if (amp.dspAlive) {
        if (amp.firmwareIdVersion > 0x001C00) {
            sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxSpeakerOutputDisable,
                               cirrus::devices::cs35l41::registers::kStatusMailboxRunning);
        }
        bool paused = sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxPause,
                                         cirrus::devices::cs35l41::registers::kStatusMailboxPaused);
        ok = paused && ok;
        if (!paused) {
            amp.firmwareValidated = false;
            stopDSP(amp);
        }
    }
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2,
                            0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 0, TRACE_PLAYBACK) &&
         ok;
    uint32_t pwr1 = 0, pwr2 = 0, volume = 0, gain = 0;
    bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &pwr1, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &volume, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PLAYBACK);
    bool safe = ok && pdnDone && readable && !(pwr1 & 1) &&
                !(pwr2 & (0x00003001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) && volume == 0x0000A678 && gain == 0;
    amp.playbackActive = !safe;
    if (!safe) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK, cirrus::devices::cs35l41::registers::kPowerControl2, 0, pwr2);
    } else if (!amp.playbackFaulted) {
        amp.cleanupAttempts = 0;
        markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Playback_Verdict_%s", amp.name);
    OSString* verdict = OSString::withCString(!safe                 ? "CLEANUP_UNVERIFIED"
                                              : amp.playbackFaulted ? "FAULT_LATCHED_OUTPUT_DISABLED"
                                                                    : "SAFE_IDLE_VERIFIED");
    if (verdict) {
        setProperty(property, verdict);
        verdict->release();
    }
    publishDriverVerdict();
    return safe;
}

// Run on the timer workloop with power and teardown serialized.
// HDA activity permits a playback attempt; it is not proof of a valid route.
// I/O, DSP and protection failures remain latched. Only an isolated PLL loss
// may recover on a selected speaker route. The compatibility fallback uses a
// bounded recovery policy because it cannot observe the selected output.
bool CirrusAudioFixup::stopRuntimePlayback(AmplifierState& amp) {
    using namespace cirrus::devices::cs35l41;
    if (amp.model != cirrus::core::CodecModel::CS35L41) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
        return false;
    }
    auto& state = amp.shutdownTransition;
    if (!state.pending()) {
        state = {};
        amp.playbackTransition = {};
        ++amp.cleanupAttempts;
        amp.playbackStableCount = 0;
        setDiagnosticStage(amp, STAGE_PLAYBACK_CLEANUP);
    }
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    FixupRegisterIOAdapter io(this, amp, TRACE_PLAYBACK);
    auto result = Playback::shutdown(io, state, now / 1000000, amp.dspAlive, amp.firmwareIdVersion, playbackTimeMilliseconds);
    if (result == PlaybackProgress::Pending) {
        // This flag means cleanup is unverified, not that audio is unmuted.
        amp.playbackActive = true;
        return false;
    }
    if (state.haltDsp) {
        amp.firmwareValidated = false;
        stopDSP(amp);
    }
    bool safe = result == PlaybackProgress::Prepared;
    amp.playbackActive = !safe;
    if (safe && amp.clockWaitAfterCleanup && amp.diagnostic.latestFailure == DIAG_POWER_UP_TIMEOUT) {
        if (amp.pllRecoveryAttempts > 0)
            --amp.pllRecoveryAttempts;
        amp.pllRecoveryPending = true;
        amp.pllRetryCooldown = 20;
        recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, registers::kRegIrq1RawStatus3, 2, 0);
    }
    amp.clockWaitAfterCleanup = false;
    if (!safe) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, state.failure, state.failureRegister, 0, state.actual, mLastTransferReturn);
    } else if (!amp.playbackFaulted) {
        amp.cleanupAttempts = 0;
        markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Playback_Verdict_%s", amp.name);
    OSString* verdict = OSString::withCString(!safe ? "CLEANUP_UNVERIFIED" : amp.playbackFaulted ?
                                             "FAULT_LATCHED_OUTPUT_DISABLED" : "SAFE_IDLE_VERIFIED");
    if (verdict) {
        setProperty(property, verdict);
        verdict->release();
    }
    publishDriverVerdict();
    return safe;
}

bool CirrusAudioFixup::setupAudioEvents() {
    if (mAudioEvents || !mHdaPci || !cirrus::platform::hda::AudioEventSource::hooksReady())
        return mAudioEvents != nullptr;
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

// Called by the event source on this service's workloop, never directly from
// AppleHDA. Power changes and teardown share the same gate as amplifier I/O.
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

void CirrusAudioFixup::runBackgroundMonitor() {
    if (mStopping || !mPowerAvailable || mNeedsReinitialization || mDebugPhaseHalted)
        return;
    bool hdaStreamActive = synchronizeHdaStream();
    setupAudioEvents();
    if (mAudioEvents)
        mAudioState = mAudioEvents->snapshot();
    const uint32_t routeGeneration = mAudioState.generation;
    const bool eventRoute = mAudioState.routeKnown;
    const bool routePermitsPlayback = !eventRoute || (mAudioState.speakers &&
                                     (!mAudioState.engineKnown || mAudioState.engineRunning));
    bool hasAudio = mHdaState.observed && hdaStreamActive && routePermitsPlayback;
    for (size_t i = 0; i < mAmpCount; ++i) {
        const auto& peer = mAmps[i];
        if (peer.present && (!peer.initialized ||
            (peer.playbackFaulted && (!peer.pllRecoveryPending || peer.pllRecoveryAttempts >= 3))))
            hasAudio = false;
    }
    uint64_t nowMs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowMs);
    nowMs /= 1000000;
    for (size_t i = 0; i < mAmpCount; ++i) {
        AmplifierState& amp = mAmps[i];
        if (!amp.present)
            continue;
        if (amp.model != cirrus::core::CodecModel::CS35L41) {
            // Initialization dispatch is not enough: runtime must also keep
            // unimplemented chips away from CS35L41 register transactions.
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
            continue;
        }
        if (amp.shutdownTransition.pending()) {
            stopRuntimePlayback(amp);
            continue;
        }
        if (amp.playbackTransition.pending() &&
            (!hasAudio || amp.playbackTransition.generation != routeGeneration ||
             amp.playbackTransition.streamTag != mHdaState.lastStreamTag ||
             amp.playbackTransition.streamFormat != mHdaState.lastFormat)) {
            stopRuntimePlayback(amp);
            continue;
        }
        // A completed output/engine change is normal lifecycle input, not a
        // hardware fault. Stop immediately instead of waiting for two timer
        // observations or manufacturing PLL errors while headphones play.
        if (eventRoute && !routePermitsPlayback) {
            if (amp.playbackActive && amp.cleanupAttempts < 3)
                stopRuntimePlayback(amp);
            if (amp.pllRecoveryPending && amp.diagnostic.latestFailure == DIAG_PLL_UNLOCKED && !amp.playbackActive) {
                amp.playbackFaulted = false;
                amp.pllRecoveryPending = false;
                amp.pllRecoveryAttempts = 0;
            }
            continue;
        }
        if (amp.playbackFaulted) {
            if (amp.playbackActive && amp.cleanupAttempts < 3)
                stopRuntimePlayback(amp);
            if (!amp.playbackActive && hasAudio && amp.initialized && amp.pllRecoveryPending &&
                amp.pllRecoveryAttempts < 3 && amp.diagnostic.latestFailure == DIAG_PLL_UNLOCKED && checkProtectionStatus(amp)) {
                // The selected speaker route authorizes preparation. Idle PLL
                // status is not a readiness gate: this hardware can report
                // unlocked until GLOBAL_EN starts the power-up sequence.
                // Only the compatibility fallback retains timeout backoff;
                // event-driven route changes never inherit headphone cooldown.
                if (!eventRoute && amp.pllRetryCooldown > 0) {
                    --amp.pllRetryCooldown;
                    continue;
                }
                amp.pllRetryCooldown = 0;
                amp.playbackFaulted = false;
                amp.pllRecoveryPending = false;
                ++amp.pllRecoveryAttempts;
                amp.cleanupAttempts = 0;
            } else {
                continue;
            }
        }

        if (amp.initialized && !amp.playbackActive && mHdaState.observed && !hdaStreamActive) {
            amp.playbackStableCount = 0;
            amp.pllRetryCooldown = 0;
            continue;
        }

        uint32_t pwrCtrl1 = 0;
        uint32_t pllLockSts = 0;
        uint32_t mbox2 = 0;
        uint32_t timestamp = 0;

        if (!readRegister(amp, 0x00002014, &pwrCtrl1, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }

        bool pllReadable = readRegister(amp, 0x00010098, &pllLockSts, TRACE_DUMP);
        if (!pllReadable) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }
        bool globalEn = (pwrCtrl1 & 0x01) != 0;
        bool pllLock = pllReadable && (pllLockSts & 0x00000002) != 0;

        if (!readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }

        if (!amp.initialized) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }

        if (!checkProtectionStatus(amp)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }

        if (!readRegister(amp, 0x025C0800, &timestamp, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            continue;
        }

        if (amp.playbackActive && hasAudio) {
            const bool dspMode = amp.monitorCount >= 1;
            uint32_t pwr2 = 0, core = 0, halo = 0;
            bool powerReadable = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK);
            bool dspHealthy = bootArgEnabled("-cirrusnodsp");
            if (dspMode) {
                dspHealthy = amp.dspAlive && amp.firmwareValidated && amp.haloStateRegister &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core, TRACE_PLAYBACK) &&
                             readRegister(amp, amp.haloStateRegister, &halo, TRACE_PLAYBACK) &&
                             (core & (cirrus::devices::cs35l41::registers::kValHaloCoreEnable |
                                      cirrus::devices::cs35l41::registers::kValHaloCoreReset)) ==
                                 cirrus::devices::cs35l41::registers::kValHaloCoreEnable &&
                             halo == 2 && mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxRunning;
            }
            if (!globalEn || !pllLock || !powerReadable ||
                (pwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) != (dspMode ? 0x3001U : 1U) || !dspHealthy) {
                CIRRUS_ERR("ACTIVE_STATE_LOST amp=%s pwr1=0x%08X pwr2=0x%08X pll=%d mbox=%u core=0x%08X halo=%u", amp.name, pwrCtrl1, pwr2,
                           pllLock, mbox2, core, halo);
                if (!pllLock) {
                    recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, 2, pllLockSts);
                    amp.pllRetryCooldown = 4;
                    amp.pllRecoveryPending = globalEn && powerReadable && dspHealthy &&
                        (pwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) ==
                            (dspMode ? 0x3001U : 1U);
                } else {
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::cs35l41::registers::kPowerControl2,
                                            dspMode ? 0x3001U : 1U, pwr2);
                }
                amp.playbackFaulted = true;
                stopRuntimePlayback(amp);
                continue;
            }
        }

        bool playbackTransition = hasAudio != amp.playbackActive;
        bool heartbeat = (++amp.monitorLogCountdown >= (hasAudio ? 50U : 40U));
        if (playbackTransition || heartbeat) {
            uint32_t aspEnables = 0, spRate = 0, spFmt = 0, dacSrc = 0, ampVol = 0;
            bool snapshotReadable =
                readRegister(amp, 0x00004800, &aspEnables, TRACE_DUMP) && readRegister(amp, 0x00004804, &spRate, TRACE_DUMP) &&
                readRegister(amp, 0x00004808, &spFmt, TRACE_DUMP) && readRegister(amp, 0x00004C00, &dacSrc, TRACE_DUMP) &&
                readRegister(amp, 0x00006000, &ampVol, TRACE_DUMP);
            if (!snapshotReadable) {
                amp.playbackFaulted = true;
                stopRuntimePlayback(amp);
                continue;
            }

            CIRRUS_LOG("background monitor %s: global_en=%d pll_lock=%d mbox2=0x%08X ts=0x%08X->0x%08X audio=%d active=%d "
                       "hda_sd=%u tag=%u fmt=0x%04X sp_en=0x%08X sp_rate=0x%08X sp_fmt=0x%08X dac_src=0x%08X vol=0x%08X mode=%s",
                       amp.name, globalEn, pllLock, mbox2, amp.lastTimestamp, timestamp, hasAudio, amp.playbackActive,
                       mHdaState.lastDescriptor, mHdaState.lastStreamTag, mHdaState.lastFormat, aspEnables, spRate, spFmt, dacSrc, ampVol,
                       (amp.monitorCount >= 1) ? "DSP" : "BYPASS");
            amp.monitorLogCountdown = 0;
        }

        amp.lastTimestamp = timestamp;

        if (hasAudio && !amp.playbackActive) {
            using namespace cirrus::devices::cs35l41;
            auto& transition = amp.playbackTransition;
            const bool dspMode = amp.monitorCount >= 1;
            if (amp.model != cirrus::core::CodecModel::CS35L41 ||
                (!dspMode && !bootArgEnabled("-cirrusnodsp")) ||
                (dspMode && (!amp.dspAlive || !amp.firmwareValidated))) {
                amp.playbackFaulted = true;
                recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT);
                stopRuntimePlayback(amp);
                continue;
            }
            if (transition.phase == PlaybackPhase::Idle) {
                transition.generation = routeGeneration;
                transition.streamTag = mHdaState.lastStreamTag;
                transition.streamFormat = mHdaState.lastFormat;
                setDiagnosticStage(amp, STAGE_PLAYBACK_PREPARE);
            }
            FixupRegisterIOAdapter io(this, amp, TRACE_PLAYBACK);
            auto result = Playback::advance(io, transition, nowMs, dspMode, amp.firmwareIdVersion, playbackTimeMilliseconds);
            if (result == PlaybackProgress::Failed) {
                const auto failure = transition.failure;
                recordDiagnosticFailure(amp, failure, transition.failureRegister,
                                        transition.expected, transition.actual, mLastTransferReturn);
                uint32_t retryPllStatus = 0;
                bool clockWait = failure == DIAG_POWER_UP_TIMEOUT && amp.pllRecoveryAttempts > 0 &&
                                 readRegister(amp, registers::kRegIrq1RawStatus3, &retryPllStatus, TRACE_PLAYBACK) &&
                                 !(retryPllStatus & 2) && checkProtectionStatus(amp);
                amp.playbackFaulted = true;
                amp.pllRecoveryPending = failure == DIAG_PLL_UNLOCKED;
                amp.pllRetryCooldown = 4;
                amp.clockWaitAfterCleanup = clockWait;
                stopRuntimePlayback(amp);
            }
        } else if (!hasAudio && amp.playbackActive) {
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 2) {
                stopRuntimePlayback(amp);
                amp.pllRetryCooldown = 0;
            }
        } else {
            amp.playbackStableCount = 0;
        }
    }

    // Stereo preparation is a barrier. Never commit one healthy endpoint
    // while its required peer is waiting or has a non-recoverable fault.
    using namespace cirrus::devices::cs35l41;
    bool pairReady = hasAudio;
    bool pairFault = false;
    bool pending = false;
    for (size_t i = 0; i < mAmpCount; ++i) {
        const auto& amp = mAmps[i];
        if (!amp.present)
            continue;
        pairReady &= amp.initialized && !amp.playbackFaulted &&
                     amp.playbackTransition.phase == PlaybackPhase::Prepared;
        pairFault |= amp.playbackFaulted;
        pending |= amp.playbackTransition.pending();
    }
    if (pairReady) {
        pairReady = synchronizeHdaStream() && mHdaState.observed;
        for (size_t i = 0; i < mAmpCount && pairReady; ++i) {
            auto& amp = mAmps[i];
            if (!amp.present)
                continue;
            uint32_t pll = 0;
            pairReady = amp.playbackTransition.streamTag == mHdaState.lastStreamTag &&
                        amp.playbackTransition.streamFormat == mHdaState.lastFormat &&
                        (!mAudioEvents || mAudioEvents->unchanged(amp.playbackTransition.generation));
            if (pairReady) {
                bool protectedState = checkProtectionStatus(amp);
                bool readable = protectedState && readRegister(amp, registers::kRegIrq1RawStatus3, &pll, TRACE_PLAYBACK);
                pairReady = readable && (pll & 2);
                if (!pairReady) {
                    amp.playbackFaulted = true;
                    if (readable) {
                        amp.pllRecoveryPending = true;
                        recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, registers::kRegIrq1RawStatus3, 2, pll);
                    }
                }
            }
        }
        for (size_t i = 0; i < mAmpCount && pairReady; ++i) {
            auto& amp = mAmps[i];
            if (!amp.present)
                continue;
            if (mAudioEvents && !mAudioEvents->unchanged(amp.playbackTransition.generation)) {
                pairReady = false;
                break;
            }
            FixupRegisterIOAdapter io(this, amp, TRACE_PLAYBACK);
            const uint32_t gain = amp.monitorCount >= 1 ? tuning::encodeGain(amp.tuningPcmGain)
                                                       : registers::kPlaybackBypassGain;
            pairReady = Playback::commit(io, amp.playbackTransition, gain, amp.monitorCount >= 1);
            if (!pairReady) {
                amp.playbackFaulted = true;
                recordDiagnosticFailure(amp, amp.playbackTransition.failure);
            }
        }
        // Hooks can publish while register transfers run. Recheck once more
        // after the final commit, then undo the whole pair if it became stale.
        if (mAudioEvents && !mAudioEvents->unchanged(routeGeneration))
            pairReady = false;
        if (pairReady) {
            for (size_t i = 0; i < mAmpCount; ++i) {
                auto& amp = mAmps[i];
                if (!amp.present)
                    continue;
                amp.playbackActive = true;
                amp.pllRecoveryAttempts = 0;
                amp.pllRecoveryPending = false;
                markDiagnosticSuccess(amp, STAGE_PLAYBACK_ACTIVE);
                char property[80];
                snprintf(property, sizeof(property), "Cirrus_Playback_Verdict_%s", amp.name);
                OSString* verdict = OSString::withCString("ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED");
                if (verdict) {
                    setProperty(property, verdict);
                    verdict->release();
                }
            }
            pending = false;
        } else {
            // A failed second commit may follow a successful first write.
            // Roll back every participant, including that first endpoint.
            for (size_t i = 0; i < mAmpCount; ++i)
                if (mAmps[i].present)
                    stopRuntimePlayback(mAmps[i]);
            pending = false;
        }
    } else if (pairFault) {
        for (size_t i = 0; i < mAmpCount; ++i)
            if (mAmps[i].present && !mAmps[i].shutdownTransition.pending() && mAmps[i].cleanupAttempts < 3 &&
                (mAmps[i].playbackActive || mAmps[i].playbackTransition.pending()))
                stopRuntimePlayback(mAmps[i]);
        pending = false;
    }

    pending = false;
    for (size_t i = 0; i < mAmpCount; ++i)
        pending |= mAmps[i].present && (mAmps[i].playbackTransition.pending() || mAmps[i].shutdownTransition.pending());
    if (mProbeTimer) {
        if (pending) {
            // Mailbox deadlines are five milliseconds. Preparation cannot
            // inherit the slower active-health or compatibility timers.
            mProbeTimer->setTimeoutMS(1);
            return;
        }
        if (eventRoute) {
            bool active = false;
            bool preparationPending = false;
            bool cleanupPending = false;
            for (size_t i = 0; i < mAmpCount; ++i) {
                active |= mAmps[i].present && mAmps[i].playbackActive;
                // One active channel must not move the other channel's
                // bounded preparation window onto the slow health timer.
                preparationPending |= mAmps[i].present && mAmps[i].initialized && !mAmps[i].playbackActive &&
                                      (!mAmps[i].playbackFaulted ||
                                       (mAmps[i].pllRecoveryPending && mAmps[i].pllRecoveryAttempts < 3));
                cleanupPending |= mAmps[i].present && mAmps[i].playbackActive &&
                                  mAmps[i].cleanupAttempts > 0 && mAmps[i].cleanupAttempts < 3;
            }
            if (routePermitsPlayback && preparationPending && mAudioPrepareRetries > 0) {
                --mAudioPrepareRetries;
                mProbeTimer->setTimeoutMS(5);
            } else if (active && routePermitsPlayback) {
                mProbeTimer->setTimeoutMS(100);
            } else if (cleanupPending) {
                mProbeTimer->setTimeoutMS(5);
            }
            // Headphone/idle state has no periodic timer. Output and engine
            // hooks wake this workloop for the next relevant transition.
            return;
        }
        // A missing selector or unavailable hooks leaves the explicit legacy
        // fallback active. It cannot distinguish headphone HDA RUN from speaker
        // HDA RUN and must not claim event-driven operation in diagnostics.
        mProbeTimer->setTimeoutMS(mHdaState.streamActive ? 100 : 50);
    }
}

void CirrusAudioFixup::probeAmp(AmplifierState& amp) {
    UInt32 deviceId = 0;
    UInt32 revisionId = 0;

    CIRRUS_LOG("amp %s probe address=0x%02X", amp.name, amp.address);

    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDeviceId, &deviceId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s device-id read failed", amp.name);
        return;
    }

    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRevisionIdRegister, &revisionId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s revision read failed", amp.name);
        return;
    }

    amp.deviceId = deviceId;
    amp.revisionId = revisionId;
    amp.present = (deviceId == cirrus::devices::cs35l41::registers::kValDeviceId);
    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver; add -cirrusdbg and report hardware profile", deviceId,
                   amp.name);
    }

    if (amp.present) {
        dumpAllRegisters(amp);
    }

    CIRRUS_LOG("amp %s devid=0x%08X revision=0x%08X present=%s", amp.name, amp.deviceId, amp.revisionId, amp.present ? "yes" : "no");
}

// Use the provider's addressed-transfer ABI and preserve its IOReturn.
// Do not fall back to the nub's default address: both stereo endpoints share
// one provider and must still target their own slave addresses.
bool CirrusAudioFixup::transferToAddress(UInt8 address, UInt8* writeBuffer, UInt16 writeLength, UInt8* readBuffer, UInt16 readLength) {
    if (address > 0x7F || (writeLength && !writeBuffer) || (readLength && !readBuffer) || (!writeLength && !readLength)) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    if (!mProvider || !mPowerAvailable || mStopping) {
        mLastTransferReturn = kIOReturnNotReady;
        if (!mProbingAmplifiers)
            CIRRUS_ERR("transfer blocked: provider=%p powered=%d stopping=%d", mProvider, mPowerAvailable, mStopping);
        return false;
    }

    setProperty("CirrusTransferCalled", kOSBooleanTrue);
    cirrus::transport::VoodooI2CTransport transport(mProvider, address);
    bool success = transport.transfer(writeBuffer, writeLength, readBuffer, readLength);
    IOReturn ret = transport.lastReturn();
    mLastTransferReturn = ret;
    setProperty("CirrusTransferRet", (uint64_t)ret, 32);
    if (!success) {
        if (!mProbingAmplifiers)
            CIRRUS_ERR("transfer address=0x%02X write=%u read=%u ret=0x%08X", address, writeLength, readLength, ret);
        return false;
    }

    return true;
}

void CirrusAudioFixup::initTraceBuffer() {
    if (mTraceLock) {
        IOLockLock(mTraceLock);
        mTraceHead = 0;
        mTraceTail = 0;
        memset(&mTraceStats, 0, sizeof(mTraceStats));
        IOLockUnlock(mTraceLock);
    }
}

void CirrusAudioFixup::recordTrace(TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen,
                                   IOReturn ret) {
    if (!mTraceLock)
        return;

    uint64_t time = 0;
    clock_get_uptime(&time);
    uint64_t timeMs = 0;
    absolutetime_to_nanoseconds(time, &timeMs);
    timeMs /= 1000000;

    IOLockLock(mTraceLock);

    if (ret == kIOReturnOffline) {
        mTraceStats.noackCount++;
    } else if (ret == kIOReturnTimeout) {
        mTraceStats.retries++;
    }

    if (isBulk) {
        if (ret == kIOReturnSuccess)
            mTraceStats.bulkSuccess++;
        else
            mTraceStats.bulkFail++;
    } else {
        if (isWrite) {
            if (ret == kIOReturnSuccess)
                mTraceStats.writeSuccess++;
            else
                mTraceStats.writeFail++;
        } else {
            if (ret == kIOReturnSuccess)
                mTraceStats.readSuccess++;
            else
                mTraceStats.readFail++;
        }
    }

    mTraceBuffer[mTraceTail].timestamp = timeMs;
    mTraceBuffer[mTraceTail].amp = ampIndex;
    mTraceBuffer[mTraceTail].isWrite = isWrite;
    mTraceBuffer[mTraceTail].isBulk = isBulk;
    mTraceBuffer[mTraceTail].reg = reg;
    mTraceBuffer[mTraceTail].value = valOrLen;
    mTraceBuffer[mTraceTail].ret = ret;
    mTraceBuffer[mTraceTail].source = source;

    mTraceTail = (mTraceTail + 1) % kTraceBufferSize;
    if (mTraceTail == mTraceHead) {
        mTraceHead = (mTraceHead + 1) % kTraceBufferSize;
    }

    IOLockUnlock(mTraceLock);
}

void CirrusAudioFixup::publishStatistics() {
    if (!mTraceLock)
        return;
    IOLockLock(mTraceLock);
    setProperty("Cirrus_Read_Success", mTraceStats.readSuccess, 32);
    setProperty("Cirrus_Read_Fail", mTraceStats.readFail, 32);
    setProperty("Cirrus_Write_Success", mTraceStats.writeSuccess, 32);
    setProperty("Cirrus_Write_Fail", mTraceStats.writeFail, 32);
    setProperty("Cirrus_Bulk_Success", mTraceStats.bulkSuccess, 32);
    setProperty("Cirrus_Bulk_Fail", mTraceStats.bulkFail, 32);
    setProperty("Cirrus_NOACK_Count", mTraceStats.noackCount, 32);
    IOLockUnlock(mTraceLock);
}

const char* CirrusAudioFixup::stageName(DriverStage stage) {
    switch (stage) {
    case STAGE_PROBE:
        return "PROBE";
    case STAGE_RESET:
        return "RESET";
    case STAGE_OTP_BOOT:
        return "OTP_BOOT";
    case STAGE_ERRATA:
        return "ERRATA";
    case STAGE_CLOCK:
        return "CLOCK";
    case STAGE_ASP:
        return "ASP";
    case STAGE_GPIO:
        return "GPIO";
    case STAGE_PLATFORM:
        return "PLATFORM";
    case STAGE_FIRMWARE_DISCOVERY:
        return "FIRMWARE_DISCOVERY";
    case STAGE_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case STAGE_DSP_BOOT:
        return "DSP_BOOT";
    case STAGE_IDLE_VERIFY:
        return "IDLE_VERIFY";
    case STAGE_HDA_DETECT:
        return "HDA_DETECT";
    case STAGE_PLAYBACK_OPEN:
        return "PLAYBACK_OPEN";
    case STAGE_PLAYBACK_PREPARE:
        return "PLAYBACK_PREPARE";
    case STAGE_PLAYBACK_ACTIVE:
        return "PLAYBACK_ACTIVE";
    case STAGE_PLAYBACK_CLEANUP:
        return "PLAYBACK_CLEANUP";
    case STAGE_PLAYBACK_CLOSE:
        return "PLAYBACK_CLOSE";
    case STAGE_SAFE_IDLE:
        return "SAFE_IDLE";
    default:
        return "NONE";
    }
}

const char* CirrusAudioFixup::failureName(DiagnosticFailure failure) {
    switch (failure) {
    case DIAG_OK:
        return "OK";
    case DIAG_PROVIDER_MISSING:
        return "PROVIDER_MISSING";
    case DIAG_I2C_TRANSFER:
        return "I2C_TRANSFER";
    case DIAG_RESET_GPIO:
        return "RESET_GPIO";
    case DIAG_DEVICE_ID:
        return "DEVICE_ID";
    case DIAG_RESET_WRITE:
        return "RESET_WRITE";
    case DIAG_OTP_TIMEOUT:
        return "OTP_TIMEOUT";
    case DIAG_OTP_BOOT_ERROR:
        return "OTP_BOOT_ERROR";
    case DIAG_AMP_PROTECTION:
        return "AMP_PROTECTION";
    case DIAG_ERRATA:
        return "ERRATA";
    case DIAG_OTP_UNPACK:
        return "OTP_UNPACK";
    case DIAG_PLL_CONFIG:
        return "PLL_CONFIG";
    case DIAG_ASP_CONFIG:
        return "ASP_CONFIG";
    case DIAG_GPIO_CONFIG:
        return "GPIO_CONFIG";
    case DIAG_PLATFORM_CONFIG:
        return "PLATFORM_CONFIG";
    case DIAG_FIRMWARE_MISSING:
        return "FIRMWARE_MISSING";
    case DIAG_FIRMWARE_PARSE:
        return "FIRMWARE_PARSE";
    case DIAG_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case DIAG_COEFFICIENT_PARSE:
        return "COEFFICIENT_PARSE";
    case DIAG_COEFFICIENT_UPLOAD:
        return "COEFFICIENT_UPLOAD";
    case DIAG_DSP_BOOT:
        return "DSP_BOOT";
    case DIAG_DSP_MAILBOX:
        return "DSP_MAILBOX";
    case DIAG_IDLE_INVARIANT:
        return "IDLE_INVARIANT";
    case DIAG_HDA_CONTROLLER:
        return "HDA_CONTROLLER";
    case DIAG_HDA_STREAM_FORMAT:
        return "HDA_STREAM_FORMAT";
    case DIAG_PLL_UNLOCKED:
        return "PLL_UNLOCKED";
    case DIAG_POWER_UP_TIMEOUT:
        return "POWER_UP_TIMEOUT";
    case DIAG_PLAYBACK_INVARIANT:
        return "PLAYBACK_INVARIANT";
    case DIAG_POWER_DOWN_TIMEOUT:
        return "POWER_DOWN_TIMEOUT";
    case DIAG_IDLE_ROLLBACK:
        return "IDLE_ROLLBACK";
    case DIAG_CALIBRATION:
        return "CALIBRATION";
    default:
        return "UNKNOWN";
    }
}

void CirrusAudioFixup::setDiagnosticStage(AmplifierState& amp, DriverStage stage) {
    DriverStage previous = amp.diagnostic.stage;
    amp.diagnostic.stage = stage;
    if (previous != stage) {
        CIRRUS_LOG("DIAG_STAGE amp=%s from=%s to=%s", amp.name, stageName(previous), stageName(stage));
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_Stage_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
}

void CirrusAudioFixup::markDiagnosticSuccess(AmplifierState& amp, DriverStage stage) {
    amp.diagnostic.stage = stage;
    amp.diagnostic.lastGoodStage = stage;
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_LastGood_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
    CIRRUS_LOG("DIAG_PASS amp=%s stage=%s", amp.name, stageName(stage));
    setDiagnosticStage(amp, stage);
}

void CirrusAudioFixup::recordDiagnosticFailure(AmplifierState& amp, DiagnosticFailure failure, UInt32 reg, UInt32 expected, UInt32 actual,
                                               IOReturn ioReturn, bool captureSnapshot) {
    DiagnosticState& diag = amp.diagnostic;
    bool first = diag.firstFailure == DIAG_OK;
    bool changed =
        diag.latestFailure != failure || diag.reg != reg || diag.actual != actual || diag.expected != expected || diag.ioReturn != ioReturn;
    if (first)
        diag.firstFailure = failure;
    diag.latestFailure = failure;
    diag.failureCount++;
    diag.reg = reg;
    diag.expected = expected;
    diag.actual = actual;
    diag.ioReturn = ioReturn;

    bool publishNow = first || changed || ((diag.failureCount % 32U) == 0);
    if (publishNow) {
        char property[96];
        OSString* value = nullptr;
        snprintf(property, sizeof(property), "Cirrus_Diag_FirstFailure_%s", amp.name);
        value = OSString::withCString(failureName(diag.firstFailure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_LatestFailure_%s", amp.name);
        value = OSString::withCString(failureName(failure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureCount_%s", amp.name);
        setProperty(property, diag.failureCount, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureReg_%s", amp.name);
        setProperty(property, reg, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Expected_%s", amp.name);
        setProperty(property, expected, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Actual_%s", amp.name);
        setProperty(property, actual, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_IOReturn_%s", amp.name);
        setProperty(property, (uint64_t)ioReturn, 32);
        publishStatistics();
    }

    if (first || changed) {
        CIRRUS_ERR("DIAG_FAIL amp=%s stage=%s code=%s count=%u reg=0x%08X expected=0x%08X actual=0x%08X io=0x%08X first=%s", amp.name,
                   stageName(diag.stage), failureName(failure), diag.failureCount, reg, expected, actual, ioReturn,
                   failureName(diag.firstFailure));
    }

    if (captureSnapshot && !mCapturingFailureSnapshot && diag.snapshotFailure != failure) {
        diag.snapshotFailure = failure;
        captureFailureSnapshot(amp, failure);
        char latestTrace[80], firstTrace[80];
        snprintf(latestTrace, sizeof(latestTrace), "Cirrus_Trace_Latest_%s", amp.name);
        snprintf(firstTrace, sizeof(firstTrace), "Cirrus_Trace_First_%s", amp.name);
        dumpTraceBuffer(latestTrace, first ? firstTrace : nullptr);
    }
}

// Snapshot collection may itself encounter bus errors.
// The recursion guard prevents those reads from starting another snapshot.
// Keep the original record and bounded trace even when the device is offline.
void CirrusAudioFixup::captureFailureSnapshot(AmplifierState& amp, DiagnosticFailure failure) {
    mCapturingFailureSnapshot = true;
    uint32_t devid = 0, revid = 0, pwr1 = 0, pwr2 = 0, pwr3 = 0, pwrSts = 0;
    uint32_t irq1 = 0, irq2 = 0, irq3 = 0, irq4 = 0, pll = 0;
    uint32_t spEn = 0, spRate = 0, spFmt = 0, spHiz = 0, rxSlot = 0, dac = 0;
    uint32_t core = 0, halo = 0, mbox1 = 0, mbox2 = 0, scratch1 = 0;
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDeviceId, &devid, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRevisionIdRegister, &revid, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &pwr1, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &pwr2, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl3, &pwr3, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerManagementStatus, &pwrSts, TRACE_DUMP);
    readRegister(amp, 0x00010010, &irq1, TRACE_DUMP);
    readRegister(amp, 0x00010014, &irq2, TRACE_DUMP);
    readRegister(amp, 0x00010018, &irq3, TRACE_DUMP);
    readRegister(amp, 0x0001001C, &irq4, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, &pll, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &spEn, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, &spRate, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, &spFmt, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, &spHiz, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFrameRxSlot, &rxSlot, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, &dac, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core, TRACE_DUMP);
    bool haloReadable = amp.haloStateRegister && readRegister(amp, amp.haloStateRegister, &halo, TRACE_DUMP);
    CIRRUS_LOG("HALO snapshot on %s: address=0x%08X readable=%d", amp.name, amp.haloStateRegister, haloReadable);
    readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP);
    readRegister(amp, cirrus::devices::cs35l41::registers::kDspMbox2Register, &mbox2, TRACE_DUMP);
    readRegister(amp, 0x02B805C0, &scratch1, TRACE_DUMP);
    mCapturingFailureSnapshot = false;

    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s code=%s hda_observed=%d hda_run=%d sd=%u tag=%u fmt=0x%04X", amp.name, failureName(failure),
               mHdaState.observed, mHdaState.streamActive, mHdaState.lastDescriptor, mHdaState.lastStreamTag, mHdaState.lastFormat);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s id=0x%08X rev=0x%08X pwr=[0x%08X 0x%08X 0x%08X] pwr_sts=0x%08X", amp.name, devid, revid, pwr1, pwr2,
               pwr3, pwrSts);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s irq=[0x%08X 0x%08X 0x%08X 0x%08X] pll_lock=%d asp=[en=0x%08X rate=0x%08X fmt=0x%08X hiz=0x%08X "
               "slot=0x%08X dac=0x%08X]",
               amp.name, irq1, irq2, irq3, irq4, (pll & 0x2) != 0, spEn, spRate, spFmt, spHiz, rxSlot, dac);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s dsp=[core=0x%08X halo=0x%08X mbox1=0x%08X mbox2=0x%08X scratch1=0x%08X]", amp.name, core, halo, mbox1,
               mbox2, scratch1);
}

// Publish initialization state, not an acoustic pass/fail result.
// A fault takes precedence over a deliberate debug halt. READY_DSP describes
// the driver's verified state; speaker routing remains a separate check.
void CirrusAudioFixup::publishDriverVerdict() {
    bool allInitialized = (mAmpCount > 0);
    bool allDsp = (mAmpCount > 0);
    bool anyFaulted = false;
    for (size_t i = 0; i < mAmpCount; ++i) {
        if (!mAmps[i].present)
            continue;
        if (!mAmps[i].initialized)
            allInitialized = false;
        if (!mAmps[i].dspAlive || !mAmps[i].firmwareValidated)
            allDsp = false;
        if (mAmps[i].playbackFaulted)
            anyFaulted = true;
    }
    const char* verdict = !mPowerAvailable                 ? "SUSPENDED"
                          : bootArgEnabled("-cirrusro")    ? "READ_ONLY"
                          : anyFaulted                     ? "FAULT_LATCHED"
                          : mDebugPhaseHalted               ? "DEBUG_PHASE_HALTED"
                          : !allInitialized                ? "FAILED_INIT"
                          : allDsp                         ? "READY_DSP"
                          : bootArgEnabled("-cirrusnodsp") ? "READY_BYPASS_EXPLICIT"
                                                           : "OUTPUT_DISABLED_DSP_UNVERIFIED";
    OSString* value = OSString::withCString(verdict);
    if (value) {
        setProperty("Cirrus_Driver_Verdict", value);
        value->release();
    }
    CIRRUS_LOG("DIAG_VERDICT driver=%s amps=%lu left[first=%s latest=%s last_good=%s] right[first=%s latest=%s last_good=%s]", verdict,
               mAmpCount, failureName(mAmps[0].diagnostic.firstFailure), failureName(mAmps[0].diagnostic.latestFailure),
               stageName(mAmps[0].diagnostic.lastGoodStage), failureName(mAmps[1].diagnostic.firstFailure),
               failureName(mAmps[1].diagnostic.latestFailure), stageName(mAmps[1].diagnostic.lastGoodStage));
}

void CirrusAudioFixup::dumpTraceBuffer(const char* propertyName, const char* mirrorPropertyName) {
    if (!mTraceLock)
        return;
    IOLockLock(mTraceLock);

    CIRRUS_LOG("--- TRACE BUFFER DUMP START ---");

    size_t bufferSize = 128 * 1024;
    char* dumpBuffer = (char*)IOMallocData(bufferSize);
    if (!dumpBuffer) {
        IOLockUnlock(mTraceLock);
        return;
    }
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;
    char lineBuffer[128];

    uint32_t curr = mTraceHead;
    while (curr != mTraceTail) {
        const TraceEntry& e = mTraceBuffer[curr];
        const char* srcStr = "OTHER";
        switch (e.source) {
        case TRACE_PROBE:
            srcStr = "Probe";
            break;
        case TRACE_DUMP:
            srcStr = "Dump";
            break;
        case TRACE_CONSISTENCY:
            srcStr = "Consist";
            break;
        case TRACE_FIRMWARE:
            srcStr = "Firmware";
            break;
        case TRACE_PLAYBACK:
            srcStr = "Playback";
            break;
        default:
            break;
        }

        const char* ampStr = (e.amp == 0) ? "LEFT" : "RIGHT";

        if (e.isBulk) {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] BULK %s 0x%05X len=%u ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.value, e.ret);
        } else {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] %s 0x%05X %s 0x%08X ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.isWrite ? "<-" : "->", e.value, e.ret);
        }

        size_t lineLen = strlen(lineBuffer);
        if (currentLen + lineLen < bufferSize - 1) {
            strlcat(dumpBuffer, lineBuffer, bufferSize);
            currentLen += lineLen;
        }

        curr = (curr + 1) % kTraceBufferSize;
    }
    CIRRUS_LOG("--- TRACE BUFFER DUMP END ---");

    OSString* strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propertyName ? propertyName : "Cirrus_Trace_Dump", strObj);
        if (mirrorPropertyName)
            setProperty(mirrorPropertyName, strObj);
        strObj->release();
    }
    IOFreeData(dumpBuffer, bufferSize);

    IOLockUnlock(mTraceLock);
}

bool CirrusAudioFixup::bulkRead(AmplifierState& amp, UInt32 reg, UInt8* data, size_t length, TraceSource source) {
    if (!data || length == 0 || length > 0xFFFFU) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 0xFFFFU, (UInt32)length, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), data, (UInt16)length);

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    recordTrace(source, ampIdx, false, true, reg, (UInt32)length, retCode);
    if (!success && !mCapturingFailureSnapshot) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, mLastTransferReturn);
    }
    mLastTransferReturn = retCode;
    return success;
}

bool CirrusAudioFixup::bulkWrite(AmplifierState& amp, UInt32 reg, const UInt8* data, size_t length, TraceSource source) {
    if ((length > 0 && !data) || length > (0xFFFFU - 4U)) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 0xFFFFU - 4U, (UInt32)length, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 stackBuffer[8];
    UInt8* writeBuffer = stackBuffer;
    bool useMalloc = (4 + length > sizeof(stackBuffer));

    if (useMalloc) {
        writeBuffer = (UInt8*)IOMallocData(4 + length);
        if (!writeBuffer) {
            recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, kIOReturnNoMemory, false);
            mLastTransferReturn = kIOReturnNoMemory;
            return false;
        }
    }

    writeBE32(writeBuffer, reg);
    if (length > 0 && data) {
        memcpy(writeBuffer + 4, data, length);
    }

    bool ret = transferToAddress(amp.address, writeBuffer, 4 + length, nullptr, 0);

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    recordTrace(source, ampIdx, true, true, reg, (uint32_t)length, retCode);
    if (!ret && !mCapturingFailureSnapshot) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, mLastTransferReturn);
    }

    if (useMalloc) {
        IOFreeData(writeBuffer, 4 + length);
    }
    mLastTransferReturn = retCode;
    return ret;
}

bool CirrusAudioFixup::readRegister(AmplifierState& amp, UInt32 reg, UInt32* value, TraceSource source) {
    if (!value) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 4, 0, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 readBuffer[4]{0};

    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), readBuffer, sizeof(readBuffer));

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;

    if (success) {
        *value = readBE32(readBuffer);
        recordTrace(source, ampIdx, false, false, reg, *value, retCode);
        return true;
    } else {
        recordTrace(source, ampIdx, false, false, reg, 0, retCode);
        if (!mCapturingFailureSnapshot) {
            recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 4, 0, mLastTransferReturn);
        }
        mLastTransferReturn = retCode;
        return false;
    }
}

bool CirrusAudioFixup::writeRegister(AmplifierState& amp, UInt32 reg, UInt32 value, TraceSource source) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, value);
    bool success = bulkWrite(amp, reg, writeBuffer, sizeof(writeBuffer), source);

    return success;
}

bool CirrusAudioFixup::updateRegisterBits(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source) {
    UInt32 currentVal = 0;
    if (!readRegister(amp, reg, &currentVal, source)) {
        CIRRUS_LOG("updateRegisterBits failed: read error at 0x%05X", reg);
        return false;
    }

    UInt32 newVal = (currentVal & ~mask) | (value & mask);

    if (newVal == currentVal) {
        return true;
    }

    return writeRegister(amp, reg, newVal, source);
}

// These polls run in thread context, never an interrupt handler.
// Yield between attempts instead of occupying a CPU for the entire timeout.
// The limit bounds attempts; bus latency and scheduling can extend wall time.
bool CirrusAudioFixup::pollRegisterBit(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs,
                                       TraceSource source) {
    UInt32 val = 0;
    for (UInt32 i = 0; i < timeoutMs; i++) {
        if (!readRegister(amp, reg, &val, source)) {
            return false;
        }
        if ((val & mask) == targetVal) {
            CIRRUS_LOG("Amp %s: poll reg=0x%05X mask=0x%X expect=0x%X elapsed=%dms iterations=%d", amp.name, reg, mask, targetVal, i + 1,
                       i + 1);
            return true;
        }
        IOSleep(1);
    }

    CIRRUS_ERR("Amp %s: pollRegisterBit timeout! reg=0x%05X, mask=0x%X, val=0x%08X", amp.name, reg, mask, val);

    UInt32 st1 = 0, st2 = 0, st3 = 0, st4 = 0, pwr_ctrl2 = 0;
    readRegister(amp, 0x10010, &st1, source);
    readRegister(amp, 0x10014, &st2, source);
    readRegister(amp, 0x10018, &st3, source);
    readRegister(amp, 0x1001C, &st4, source);
    readRegister(amp, 0x02018, &pwr_ctrl2, source);
    CIRRUS_ERR("Amp %s: DIAGNOSTICS -> ST1=0x%08X ST2=0x%08X ST3=0x%08X ST4=0x%08X PWR_CTRL2=0x%08X", amp.name, st1, st2, st3, st4,
               pwr_ctrl2);

    return false;
}

// Mailbox writes require the expected firmware acknowledgement.
// The transfer completing only confirms bus delivery. Error sentinel values
// and a response timeout leave the playback sequence unverified.
// Linux's CSPL interface reports status through physical MBOX2 (0x13004).
// Keep that contract separate from the virtual command bank: a Windows
// implementation polling 0x13024 does not establish an interchangeable ABI.
bool CirrusAudioFixup::sendMailboxCommand(AmplifierState& amp, UInt32 command, UInt32 expectedStatus) {
    if (!writeRegister(amp, 0x00013020, command, TRACE_PLAYBACK)) {
        recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, 0x00013020, command, 0, mLastTransferReturn);
        return false;
    }

    UInt32 status = 0;
    // Check immediately, then retain the full five-millisecond retry window.
    // Only a pending response needs a sleep; completed commands must not pay
    // an unconditional delay on every resume, output-enable and pause.
    for (unsigned attempt = 0; attempt <= 5; ++attempt) {
        if (attempt != 0)
            IOSleep(1);
        if (!readRegister(amp, cirrus::devices::cs35l41::registers::kDspMbox2Register, &status, TRACE_PLAYBACK)) {
            recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::cs35l41::registers::kDspMbox2Register, expectedStatus, status, mLastTransferReturn);
            return false;
        }
        if (status == 0xFFFFFFFFU || status == 0x00FFFFFFU) {
            CIRRUS_ERR("DSP mailbox reported error on %s for command %u: 0x%08X", amp.name, command, status);
            recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::cs35l41::registers::kDspMbox2Register, expectedStatus, status);
            return false;
        }
        if (status == expectedStatus)
            return true;
    }

    CIRRUS_ERR("DSP mailbox command %u timed out on %s (expected=%u status=0x%08X)", command, amp.name, expectedStatus, status);
    recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::cs35l41::registers::kDspMbox2Register, expectedStatus, status);
    return false;
}

// Recheck identity before and after software reset.
// Do not reuse a prior present flag or continue from a partially completed
// reset. Later stages need the current silicon and OTP boot result.
bool CirrusAudioFixup::initCodec(AmplifierState& amp) {
    setDiagnosticStage(amp, STAGE_PROBE);
    amp.present = false;

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    if (!device.probe(io)) {
        CIRRUS_ERR("failed to identify %s before reset", amp.name);
        return false;
    }

    amp.deviceId = device.deviceId();
    amp.revisionId = device.revisionId();
    amp.present = device.isPresent();

    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver; add -cirrusdbg and report hardware profile",
                   amp.deviceId, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
        return false;
    }

    bool deepDiag = bootArgEnabled("-cirrusdiag");
    UInt32 crcBefore = deepDiag ? calculateRegistersCRC32(amp) : 0;

    CIRRUS_LOG("amplifier %s status before reset: devid=0x%08X revid=0x%08X%s", amp.name, amp.deviceId, amp.revisionId,
               deepDiag ? " (deep diagnostics enabled)" : "");
    CIRRUS_LOG("sending soft reset to %s", amp.name);

    setDiagnosticStage(amp, STAGE_RESET);

    auto result = device.initialize(io);
    if (result == cirrus::core::AudioDevice::InitResult::ResetFailed) {
        CIRRUS_ERR("failed to send soft reset to %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_RESET_WRITE, cirrus::devices::cs35l41::registers::kRegSoftwareReset,
                                cirrus::devices::cs35l41::registers::kValSoftwareReset, 0, mLastTransferReturn);
        return false;
    }

    setDiagnosticStage(amp, STAGE_OTP_BOOT);
    if (result == cirrus::core::AudioDevice::InitResult::BootTimeout) {
        CIRRUS_ERR("otp boot complete polling failed on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_OTP_TIMEOUT, cirrus::devices::cs35l41::registers::kRegIrq1Status4,
                                cirrus::devices::cs35l41::registers::kMaskOtpBootDone, 0);
        return false;
    }

    if (result == cirrus::core::AudioDevice::InitResult::BootError) {
        UInt32 otpStatus = 0;
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Status3, &otpStatus, TRACE_PROBE);
        recordDiagnosticFailure(amp, DIAG_OTP_BOOT_ERROR, cirrus::devices::cs35l41::registers::kRegIrq1Status3, 0, otpStatus);
        return false;
    }

    if (result != cirrus::core::AudioDevice::InitResult::Success) {
        CIRRUS_ERR("failed to initialize %s", amp.name);
        return false;
    }

    UInt32 devIdAfter = 0, revIdAfter = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDeviceId, &devIdAfter) ||
        !readRegister(amp, cirrus::devices::cs35l41::registers::kRegRevisionId, &revIdAfter)) {
        CIRRUS_ERR("failed to identify %s after reset", amp.name);
        return false;
    }

    amp.deviceId = devIdAfter;
    amp.revisionId = revIdAfter;
    amp.present = (devIdAfter == cirrus::devices::cs35l41::registers::kValDeviceId);
    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver after reset; add -cirrusdbg and report hardware profile",
                   devIdAfter, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::cs35l41::registers::kValDeviceId, devIdAfter);
        return false;
    }

    UInt32 crcAfter = deepDiag ? calculateRegistersCRC32(amp) : 0;

    char propName[64];
    if (deepDiag) {
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Before_%s", amp.name);
        setProperty(propName, crcBefore, 32);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_After_%s", amp.name);
        setProperty(propName, crcAfter, 32);
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X crc=0x%08X", amp.name, devIdAfter, revIdAfter, crcAfter);
    } else {
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X", amp.name, devIdAfter, revIdAfter);
    }

    UInt32 revOnly = revIdAfter & 0xFF;

    switch (revOnly) {
    case 0xB0:
    case 0xB1:
    case 0xB2:
        CIRRUS_LOG("revision 0x%02X verified for %s", revOnly, amp.name);
        break;
    default:
        CIRRUS_LOG("unknown chip revision 0x%02X found on %s", revOnly, amp.name);
        break;
    }

    return true;
}

static uint32_t crc32Le(uint32_t crc, uint8_t const* buf, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return ~crc;
}

uint32_t CirrusAudioFixup::calculateRegistersCRC32(AmplifierState& amp) {
    uint32_t crc = 0;
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable)
            continue;
        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            crc = crc32Le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32Le(crc, (const uint8_t*)&val, 4);
        }
    }
    return crc;
}

void CirrusAudioFixup::dumpAllRegisters(AmplifierState& amp) {
    // Keep the promised read-only/probe snapshot available without verbosity.
    // Normal Release discovery must not perform an unsolicited register sweep.
    if (!gCirrusDebug && !bootArgEnabled("-cirrusro") && !bootArgStrEquals("-cirrusphase", "probe"))
        return;
    bool compact = bootArgEnabled("-cirruscompact");
    CIRRUS_LOG("starting full register dump for %s", amp.name);

    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    uint32_t successCount = 0;
    uint32_t crc = 0;

    size_t bufferSize = 16 * 1024;
    char* dumpBuffer = (char*)IOMallocData(bufferSize);
    if (!dumpBuffer)
        return;
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;

    char lineBuffer[128];

    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable)
            continue;

        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            successCount++;
            crc = crc32Le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32Le(crc, (const uint8_t*)&val, 4);

            if (compact) {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X: %08X\n", cs35l41_reg_desc[i].addr, val);
            } else {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X  %-35s = %08X\n", cs35l41_reg_desc[i].addr, cs35l41_reg_desc[i].name, val);
            }

            size_t lineLen = strlen(lineBuffer);
            if (currentLen + lineLen < bufferSize - 1) {
                strlcat(dumpBuffer, lineBuffer, bufferSize);
                currentLen += lineLen;
            }
        }
    }
    CIRRUS_LOG("dump complete: %u registers successfully read, crc32: 0x%08X", successCount, crc);

    snprintf(lineBuffer, sizeof(lineBuffer), "crc32: 0x%08X\n", crc);
    strlcat(dumpBuffer, lineBuffer, bufferSize);

    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_Dump_%s", amp.name);

    OSString* strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propName, strObj);
        strObj->release();
    }
    IOFreeData(dumpBuffer, bufferSize);
}

void CirrusAudioFixup::runTimeBasedFSMCheck(AmplifierState& amp) {
    CIRRUS_LOG("starting finite state machine check for %s", amp.name);

    uint32_t crcT0 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t0 checksum for %s: 0x%08X", amp.name, crcT0);

    IOSleep(10);
    uint32_t crcT1 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t1 checksum for %s: 0x%08X", amp.name, crcT1);

    IOSleep(20);
    uint32_t crcT5 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t5 checksum for %s: 0x%08X", amp.name, crcT5);

    IOSleep(50);
    uint32_t crcT30 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t30 checksum for %s: 0x%08X", amp.name, crcT30);

    if (crcT0 == crcT1 && crcT1 == crcT5 && crcT5 == crcT30) {
        CIRRUS_LOG("fsm state is stable on %s", amp.name);
    } else {
        CIRRUS_LOG("fsm state is changing on %s", amp.name);
    }
}

bool CirrusAudioFixup::unlockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x00000055);
    bool ret2 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x000000AA);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to unlock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys unlocked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::lockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x000000CC);
    bool ret2 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x00000033);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to lock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys locked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::initializeHardwareErrata(AmplifierState& amp) {
    CIRRUS_LOG("starting hardware errata initialization for %s", amp.name);

    bool result = false;
    bool deepDiag = bootArgEnabled("-cirrusdiag");
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc);
    size_t allocSize = numRegs * sizeof(UInt32);

    char propName[64] = {0};
    UInt32 crcUnlock = 0;
    UInt32 crcErrata = 0;
    UInt32 crcOtp = 0;
    UInt32 crcLock = 0;

    UInt32* snapshot0 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot1 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot2 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot3 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;

    if (deepDiag && (!snapshot0 || !snapshot1 || !snapshot2 || !snapshot3)) {
        CIRRUS_ERR("failed to allocate memory for register snapshots on %s", amp.name);
        goto cleanup;
    }

    if (deepDiag)
        snapshotRegisters(amp, snapshot0);

    if (!unlockTestKey(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot1);
        crcUnlock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
        setProperty(propName, crcUnlock, 32);
        CIRRUS_LOG("checksum after unlock on %s: 0x%08X", amp.name, crcUnlock);
    }

    if (!applyErrataPatch(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot2);
        CIRRUS_LOG("register diffs after applying errata patch on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot1, snapshot2);
        crcErrata = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
        setProperty(propName, crcErrata, 32);
        CIRRUS_LOG("checksum after errata patch on %s: 0x%08X", amp.name, crcErrata);
    }

    if (!unpackOTP(amp)) {
        CIRRUS_ERR("otp unpacking failed on %s, locking test key as rollback", amp.name);
        recordDiagnosticFailure(amp, DIAG_OTP_UNPACK);
        lockTestKey(amp);
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot3);
        CIRRUS_LOG("register diffs after unpacking otp on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot2, snapshot3);
        crcOtp = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
        setProperty(propName, crcOtp, 32);
        CIRRUS_LOG("checksum after otp unpack on %s: 0x%08X", amp.name, crcOtp);
    }

    if (!lockTestKey(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        crcLock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
        setProperty(propName, crcLock, 32);
        CIRRUS_LOG("checksum after locking test keys on %s: 0x%08X", amp.name, crcLock);
    }

    CIRRUS_LOG("hardware errata init completed for %s", amp.name);
    result = true;

cleanup:
    if (!result) {
        if (!lockTestKey(amp))
            amp.playbackFaulted = true;
    }
    if (snapshot0)
        IOFreeData(snapshot0, allocSize);
    if (snapshot1)
        IOFreeData(snapshot1, allocSize);
    if (snapshot2)
        IOFreeData(snapshot2, allocSize);
    if (snapshot3)
        IOFreeData(snapshot3, allocSize);

    return result;
}

bool CirrusAudioFixup::applyErrataPatch(AmplifierState& amp) {
    const ErrataTable errataTables[] = {{0xB2, cs35l41_revb2_errata_patch, sizeof(cs35l41_revb2_errata_patch) / sizeof(ErrataPatch)}};

    UInt32 revOnly = amp.revisionId & 0xFF;
    const ErrataTable* tableToApply = nullptr;

    for (size_t i = 0; i < sizeof(errataTables) / sizeof(ErrataTable); i++) {
        if (errataTables[i].revid == revOnly) {
            tableToApply = &errataTables[i];
            break;
        }
    }

    if (!tableToApply) {
        CIRRUS_ERR("no errata patches found for revision 0x%02X on %s", revOnly, amp.name);
        return false;
    }

    CIRRUS_LOG("applying %lu errata patches for revision 0x%02X on %s", tableToApply->numPatches, revOnly, amp.name);

    for (size_t i = 0; i < tableToApply->numPatches; i++) {
        if (!writeRegister(amp, tableToApply->patches[i].reg, tableToApply->patches[i].value)) {
            CIRRUS_ERR("failed to apply errata patch at register 0x%08X on %s", tableToApply->patches[i].reg, amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA, tableToApply->patches[i].reg, tableToApply->patches[i].value, 0, mLastTransferReturn);
            return false;
        }
    }

    if (!updateRegisterBits(amp, 0x02BC1000, cirrus::devices::cs35l41::registers::kValHaloCoreEnable, 0x00000000)) {
        CIRRUS_ERR("failed to disable dsp core in core ctrl on %s", amp.name);
        return false;
    }

    return true;
}

bool CirrusAudioFixup::unpackOTP(AmplifierState& amp) {
    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
    if (!device.unpackOTP(io)) {
        CIRRUS_ERR("failed to unpack OTP on %s", amp.name);
        return false;
    }
    CIRRUS_LOG("otp unpacking complete on %s", amp.name);
    return true;
}

void CirrusAudioFixup::snapshotRegisters(AmplifierState& amp, UInt32* snapshot) {
    if (!amp.present)
        return;
    for (size_t i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
        UInt32 val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val)) {
            snapshot[i] = val;
        } else {
            snapshot[i] = 0xFFFFFFFF;
        }
    }
}

void CirrusAudioFixup::compareRegisterSnapshots(AmplifierState& amp, const UInt32* oldSnapshot, const UInt32* newSnapshot) {
    if (!amp.present)
        return;
    CIRRUS_LOG("comparing register diffs on %s:", amp.name);
    int diffCount = 0;

    for (size_t i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
        if (oldSnapshot[i] != newSnapshot[i] && oldSnapshot[i] != 0xFFFFFFFF && newSnapshot[i] != 0xFFFFFFFF) {
            CIRRUS_LOG("Amp %s: [DIFF] %s (0x%08X) changed from 0x%08X to 0x%08X", amp.name, cs35l41_reg_desc[i].name,
                       cs35l41_reg_desc[i].addr, oldSnapshot[i], newSnapshot[i]);
            diffCount++;
        }
    }

    CIRRUS_LOG("Amp %s: Total %d registers changed.", amp.name, diffCount);
}

bool CirrusAudioFixup::applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sequence[i].updateBits) {
            if (!updateRegisterBits(amp, sequence[i].reg, sequence[i].mask, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        } else {
            if (!writeRegister(amp, sequence[i].reg, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        }
        if (sequence[i].delayUs > 0) {
            IODelay(sequence[i].delayUs);
        }
    }
    return true;
}

// Configure the profile's reference clock without demanding idle-time lock.
// BCLK can be absent while HDA is idle. Playback performs the lock check
// again before the output stage is unmuted.
bool CirrusAudioFixup::applyPLL(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    bool pllLocked = false;
    if (!device.setupPLL(io, &pllLocked)) {
        return false;
    }

    if (!pllLocked) {
        CIRRUS_LOG("warning: PLL did NOT lock on %s. I2S clocks might be inactive.", amp.name);
    } else {
        CIRRUS_LOG("PLL lock verified successfully on %s", amp.name);
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (amp.channel == cirrus::core::AudioChannel::Right) {
        setProperty("Cirrus_PLL_CRC_right", crcAfter, 32);
        setProperty("Cirrus_PLL_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_PLL_CRC_left", crcAfter, 32);
        setProperty("Cirrus_PLL_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

bool CirrusAudioFixup::applyASP(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    if (!device.setupASP(io, amp.channel == cirrus::core::AudioChannel::Right, amp.firmwareValidated)) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_ASP_CRC_right", crcAfter, 32);
        setProperty("Cirrus_ASP_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_ASP_CRC_left", crcAfter, 32);
        setProperty("Cirrus_ASP_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

bool CirrusAudioFixup::applyGPIO(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    if (!device.setupGPIO(io)) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_GPIO_CRC_right", crcAfter, 32);
        setProperty("Cirrus_GPIO_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_GPIO_CRC_left", crcAfter, 32);
        setProperty("Cirrus_GPIO_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

// Prefer the controller behind AppleHDA, then inspect PCI candidates.
// Penalize display-audio devices to avoid watching HDMI instead of speakers.
// The returned service is retained; the caller must release or cache it.
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
                    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, parent);
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
        IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, service);
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

IOService* CirrusAudioFixup::getAudioController() {
    return audioController();
}

static uint32_t calculateCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

bool CirrusAudioFixup::stopDSP(AmplifierState& amp) {
    amp.dspAlive = false;
    amp.monitorCount = 0;
    bool halted = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
                                     cirrus::devices::cs35l41::registers::kValHaloCoreEnable, 0);
    bool reset = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CoreSoftReset, 1, 1);
    IODelay(1000);
    uint32_t core = 0;
    return halted && reset && readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core) &&
           !(core & cirrus::devices::cs35l41::registers::kValHaloCoreEnable);
}

// Select board and channel tuning, not just a compatible firmware container.
// SSID translations are explicit quirks. An override is a bring-up tool and
// does not establish that another board has the same speakers or boost circuit.
void CirrusAudioFixup::discoverFirmware(AmplifierState& amp) {
    CIRRUS_LOG("discovering firmware for amplifier: %s", amp.name);

    amp.firmwareValidated = false;
    amp.tuningPcmGain = 17;
    char tuningProperty[80];
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_Source_%s", amp.name);
    removeProperty(tuningProperty);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_PCM_Gain_%s", amp.name);
    removeProperty(tuningProperty);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_CRC32_%s", amp.name);
    removeProperty(tuningProperty);
    amp.wmfwData = amp.binData = nullptr;
    amp.wmfwSize = amp.binSize = 0;

    char propStatus[64];
    snprintf(propStatus, sizeof(propStatus), "Cirrus_Phase5A_Status_%s", amp.name);

    if (bootArgEnabled("-cirrusnodsp")) {
        CIRRUS_LOG("Non-DSP Bypass mode forced via boot-arg for %s", amp.name);
        OSString* statusStr = OSString::withCString("BYPASS");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    uint32_t subVendor = 0;
    uint32_t subDevice = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t revisionId = 0;

    struct SsidTranslationQuirk {
        uint16_t fromVendor;
        uint16_t fromDevice;
        uint16_t toVendor;
        uint16_t toDevice;
    };
    static const SsidTranslationQuirk kSsidQuirks[] = {
        {0x17AA, 0x382B, 0x17AA, 0x3847},
    };
    auto firmwareResourceExists = [](uint16_t vendor, uint16_t device) -> bool {
        for (size_t i = 0; i < cs35l41FirmwareCount; ++i) {
            if (cs35l41Firmware[i].subsystemVendor == vendor && cs35l41Firmware[i].subsystemDevice == device)
                return true;
        }
        for (size_t i = 0; i < sizeof(kSsidQuirks) / sizeof(kSsidQuirks[0]); ++i) {
            if (kSsidQuirks[i].fromVendor == vendor && kSsidQuirks[i].fromDevice == device)
                return true;
            if (kSsidQuirks[i].toVendor == vendor && kSsidQuirks[i].toDevice == device)
                return true;
        }
        return false;
    };

    IOService* audioController = getAudioController();
    if (audioController) {
        IOPCIDevice* pciDev = OSDynamicCast(IOPCIDevice, audioController);
        if (pciDev) {
            vendorId = pciDev->configRead16(kIOPCIConfigVendorID);
            deviceId = pciDev->configRead16(kIOPCIConfigDeviceID);
            subVendor = pciDev->configRead16(kIOPCIConfigSubSystemVendorID);
            subDevice = pciDev->configRead16(kIOPCIConfigSubSystemID);
            revisionId = pciDev->configRead8(kIOPCIConfigRevisionID);
        }

        if (subVendor == 0 || subDevice == 0) {
            OSData* subVenData = OSDynamicCast(OSData, audioController->getProperty("subsystem-vendor-id"));
            if (subVenData && subVenData->getLength() >= 4)
                subVendor = *((uint32_t*)subVenData->getBytesNoCopy()) & 0xFFFF;
            else if (subVenData && subVenData->getLength() >= 2)
                subVendor = *((uint16_t*)subVenData->getBytesNoCopy());

            OSData* subDevData = OSDynamicCast(OSData, audioController->getProperty("subsystem-id"));
            if (subDevData && subDevData->getLength() >= 4) {
                uint32_t rawSub = *((uint32_t*)subDevData->getBytesNoCopy());
                if (subVendor == 0 && rawSub > 0xFFFF) {
                    uint16_t low = rawSub & 0xFFFF;
                    uint16_t high = rawSub >> 16;
                    if (firmwareResourceExists(low, high) && !firmwareResourceExists(high, low)) {
                        subVendor = low;
                        subDevice = high;
                    } else {
                        subVendor = high;
                        subDevice = low;
                    }
                } else {
                    subDevice = rawSub & 0xFFFF;
                }
            } else if (subDevData && subDevData->getLength() >= 2) {
                subDevice = *((uint16_t*)subDevData->getBytesNoCopy());
            }
        }

        if (vendorId == 0 || deviceId == 0) {
            OSData* venData = OSDynamicCast(OSData, audioController->getProperty("vendor-id"));
            if (venData && venData->getLength() >= 4)
                vendorId = *((uint32_t*)venData->getBytesNoCopy()) & 0xFFFF;
            else if (venData && venData->getLength() >= 2)
                vendorId = *((uint16_t*)venData->getBytesNoCopy());

            OSData* devData = OSDynamicCast(OSData, audioController->getProperty("device-id"));
            if (devData && devData->getLength() >= 4)
                deviceId = *((uint32_t*)devData->getBytesNoCopy()) & 0xFFFF;
            else if (devData && devData->getLength() >= 2)
                deviceId = *((uint16_t*)devData->getBytesNoCopy());
        }

        if (revisionId == 0) {
            OSData* revData = OSDynamicCast(OSData, audioController->getProperty("revision-id"));
            if (revData && revData->getLength() >= 4)
                revisionId = *((uint32_t*)revData->getBytesNoCopy());
            else if (revData && revData->getLength() >= 1)
                revisionId = *((uint8_t*)revData->getBytesNoCopy());
        }
        char propPath[64];
        snprintf(propPath, sizeof(propPath), "Cirrus_PCI_Path_%s", amp.name);
        char pathStr[512] = {0};
        int pathLen = sizeof(pathStr);
        if (audioController->getPath(pathStr, &pathLen, gIOServicePlane)) {
            OSString* pStr = OSString::withCString(pathStr);
            if (pStr) {
                setProperty(propPath, pStr);
                pStr->release();
            }
        }

        OSString* pciDebug = OSDynamicCast(OSString, audioController->getProperty("pcidebug"));
        if (pciDebug) {
            char propBDF[64];
            snprintf(propBDF, sizeof(propBDF), "Cirrus_PCI_BDF_%s", amp.name);
            setProperty(propBDF, pciDebug);
        }

        char prop[64];
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Vendor_%s", amp.name);
        setProperty(prop, (uint64_t)vendorId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Device_%s", amp.name);
        setProperty(prop, (uint64_t)deviceId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Revision_%s", amp.name);
        setProperty(prop, (uint64_t)revisionId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubVendor_%s", amp.name);
        setProperty(prop, (uint64_t)subVendor, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubDevice_%s", amp.name);
        setProperty(prop, (uint64_t)subDevice, 32);

        audioController->release();
    }

    uint32_t codecSSID = 0;
    const char* codecClasses[] = {"IOHDACodecDevice", "AppleHDACodecGeneric", "AppleHDACodec"};
    for (size_t c = 0; c < 3 && codecSSID == 0; c++) {
        OSDictionary* match = serviceMatching(codecClasses[c]);
        if (!match)
            continue;
        OSIterator* iter = getMatchingServices(match);
        match->release();
        if (!iter)
            continue;
        IORegistryEntry* entry;
        while ((entry = OSDynamicCast(IORegistryEntry, iter->getNextObject()))) {
            OSData* subData = OSDynamicCast(OSData, entry->getProperty("subsystem-id"));
            if (!subData)
                subData = OSDynamicCast(OSData, entry->getProperty("SubsystemID"));
            if (subData && subData->getLength() >= 4) {
                codecSSID = *(const uint32_t*)subData->getBytesNoCopy();
                break;
            }
            OSNumber* subNum = OSDynamicCast(OSNumber, entry->getProperty("subsystem-id"));
            if (!subNum)
                subNum = OSDynamicCast(OSNumber, entry->getProperty("SubsystemID"));
            if (subNum) {
                codecSSID = subNum->unsigned32BitValue();
                break;
            }
        }
        iter->release();
    }

    uint32_t ssid = 0;
    if (codecSSID != 0) {
        ssid = codecSSID;
        subVendor = ssid >> 16;
        subDevice = ssid & 0xFFFF;
    } else {
        ssid = (subVendor << 16) | subDevice;
    }

    for (size_t q = 0; q < sizeof(kSsidQuirks) / sizeof(kSsidQuirks[0]); ++q) {
        if (subVendor == kSsidQuirks[q].fromVendor && subDevice == kSsidQuirks[q].fromDevice) {
            subVendor = kSsidQuirks[q].toVendor;
            subDevice = kSsidQuirks[q].toDevice;
            ssid = (uint32_t(subVendor) << 16) | subDevice;
            CIRRUS_LOG("Applied SSID translation quirk: %04X:%04X -> %04X:%04X", kSsidQuirks[q].fromVendor, kSsidQuirks[q].fromDevice,
                       subVendor, subDevice);
            break;
        }
    }

    uint32_t explicitSSID = 0;
    if (PE_parse_boot_argn("-cirrusssid", &explicitSSID, sizeof(explicitSSID))) {
        ssid = explicitSSID;
        subVendor = ssid >> 16;
        subDevice = ssid & 0xFFFF;
        CIRRUS_LOG("Explicit firmware SSID override: 0x%08X", ssid);
    }

    uint32_t spkid = 1;
    bool explicitSpeaker = PE_parse_boot_argn("-cirrusspkid", &spkid, sizeof(spkid));
    // The compiled speaker default preserves the existing reference profile.
    // It is not a GPIO/ACPI measurement of the physical speaker vendor.
    const char* speakerSource = explicitSpeaker ? "BOOT_ARGUMENT" : "COMPILED_DEFAULT_UNVERIFIED";
    CIRRUS_LOG("Firmware identity %s: SSID=0x%08X speaker=%u source=%s", amp.name, ssid, spkid,
               speakerSource);

    char speakerProperty[80];
    snprintf(speakerProperty, sizeof(speakerProperty), "Cirrus_Speaker_ID_%s", amp.name);
    setProperty(speakerProperty, uint64_t(spkid), 32);
    snprintf(speakerProperty, sizeof(speakerProperty), "Cirrus_Speaker_ID_Source_%s", amp.name);
    setProperty(speakerProperty, speakerSource);

    char propSSID[64];
    snprintf(propSSID, sizeof(propSSID), "Cirrus_SSID_%s", amp.name);
    setProperty(propSSID, (uint64_t)ssid, 32);

    const FirmwareResource* foundRes = nullptr;
    for (size_t i = 0; i < cs35l41FirmwareCount; i++) {
        if (cs35l41Firmware[i].subsystemVendor == subVendor && cs35l41Firmware[i].subsystemDevice == subDevice &&
            (cs35l41Firmware[i].spkid == spkid || cs35l41Firmware[i].spkid == 0)) {
            foundRes = &cs35l41Firmware[i];
            break;
        }
    }

    if (!foundRes) {
        CIRRUS_ERR("No firmware resource matched Subsystem ID 0x%08X (vendor=0x%04X, device=0x%04X, spkid=%u) on %s", ssid, subVendor,
                   subDevice, spkid, amp.name);
        if (!gCirrusDebug) {
            CIRRUS_ERR("To dump full hardware profile, add '-cirrusdbg' to boot-args and reboot");
            CIRRUS_ERR("Report your hardware profile to: https://github.com/hoaug-tran/CirrusAudioFixup/issues");
        } else {
            IOLog(CIRRUS_LOG_PREFIX "=== CIRRUS AUDIO HARDWARE PROFILE DUMP ===\n");
            IOLog(CIRRUS_LOG_PREFIX "SSID: 0x%08X (Vendor: 0x%04X, Device: 0x%04X)\n", ssid, subVendor, subDevice);
            IOLog(CIRRUS_LOG_PREFIX "Amp: %s, I2C Address: 0x%02X\n", amp.name, amp.address);
            IOLog(CIRRUS_LOG_PREFIX "Hardware ID: DEVID=0x%08X REVID=0x%08X\n", amp.deviceId, amp.revisionId);
            IOLog(CIRRUS_LOG_PREFIX "Speaker ID: %u (explicit=%d)\n", spkid, explicitSpeaker ? 1 : 0);
            IOLog(CIRRUS_LOG_PREFIX "HDA Stream: Tag=%u Format=0x%04X\n", mHdaState.lastStreamTag, mHdaState.lastFormat);
            IOLog(CIRRUS_LOG_PREFIX "=== END HARDWARE PROFILE DUMP ===\n");
        }
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 0, ssid);
        OSString* statusStr = OSString::withCString("UNSUPPORTED_SSID");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfw == nullptr || foundRes->bin == nullptr) {
        CIRRUS_ERR("missing firmware binary or tuning files for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("FILE_NOT_FOUND");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfwSize == 0 || foundRes->binSize == 0) {
        CIRRUS_ERR("invalid firmware sizes for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if ((foundRes->wmfwSize % 4 != 0) || (foundRes->binSize % 4 != 0)) {
        CIRRUS_ERR("firmware files alignment must be 4-byte on ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_ALIGNMENT");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfwSize >= 4 && foundRes->wmfw[0] == 'W' && foundRes->wmfw[1] == 'M' && foundRes->wmfw[2] == 'F' &&
        foundRes->wmfw[3] == 'W') {
    } else {
        CIRRUS_ERR("wmfw magic header verification failed for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    char propFW[64];
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Source_%s", amp.name);
    OSString* srcStr = OSString::withCString("EmbeddedResource");
    if (srcStr) {
        setProperty(propFW, srcStr);
        srcStr->release();
    }

    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Type_%s", amp.name);
    OSString* typeStr = OSString::withCString(foundRes->isDummy ? "Dummy" : "Production");
    if (typeStr) {
        setProperty(propFW, typeStr);
        typeStr->release();
    }

    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Size_%s", amp.name);
    setProperty(propFW, (uint64_t)foundRes->wmfwSize, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_Size_%s", amp.name);
    setProperty(propFW, (uint64_t)foundRes->binSize, 32);

    bool isRight = amp.channel == cirrus::core::AudioChannel::Right;
    const uint8_t* chanBin = (isRight && foundRes->binRight) ? foundRes->binRight : foundRes->bin;
    size_t chanBinSize = (isRight && foundRes->binRight) ? foundRes->binRightSize : foundRes->binSize;

    const uint8_t* tuningData = isRight ? foundRes->tuningRight : foundRes->tuningLeft;
    const size_t tuningSize = isRight ? foundRes->tuningRightSize : foundRes->tuningLeftSize;
    const char* tuningName = isRight ? foundRes->tuningRightName : foundRes->tuningLeftName;
    cirrus::devices::cs35l41::tuning::Parameters tuning{};
    if (!cirrus::devices::cs35l41::tuning::parse(tuningData, tuningSize, tuning)) {
        CIRRUS_ERR("invalid embedded gain tuning for %s; refusing this resource", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        setProperty(propStatus, "INVALID_GAIN_TUNING");
        return;
    }
    amp.tuningPcmGain = tuning.pcmGain;
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_PCM_Gain_%s", amp.name);
    setProperty(tuningProperty, uint64_t(amp.tuningPcmGain), 32);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_Source_%s", amp.name);
    setProperty(tuningProperty, tuning.overridden ? (tuningName ? tuningName : "EMBEDDED_BINCFG") : "LINUX_DEFAULT");
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_CRC32_%s", amp.name);
    setProperty(tuningProperty, uint64_t(tuningData ? calculateCrc32(tuningData, tuningSize) : 0), 32);

    uint32_t fwCrc = calculateCrc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculateCrc32(chanBin, chanBinSize);
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_CRC32_%s", amp.name);
    setProperty(propFW, (uint64_t)fwCrc, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_CRC32_%s", amp.name);
    setProperty(propFW, (uint64_t)binCrc, 32);

    uint32_t fwVersion = foundRes->wmfwSize >= 12 ? foundRes->wmfw[11] : 0;
    uint32_t binVersion = chanBinSize >= 12 ? CirrusFirmwareParser::readLE32(chanBin + 8) : 0;

    char propFwVer[64];
    snprintf(propFwVer, sizeof(propFwVer), "Cirrus_WMFW_Container_Version_%s", amp.name);
    setProperty(propFwVer, (uint64_t)fwVersion, 32);

    char propBinVer[64];
    snprintf(propBinVer, sizeof(propBinVer), "Cirrus_BIN_Version_%s", amp.name);
    setProperty(propBinVer, (uint64_t)binVersion, 32);

    amp.wmfwData = foundRes->wmfw;
    amp.wmfwSize = foundRes->wmfwSize;
    amp.binData = chanBin;
    amp.binSize = chanBinSize;
    amp.firmwareValidated = false;

    CIRRUS_LOG("firmware matches found: %s (size: %lu, version: %08X, crc: %08X), tuning: %s (size: %lu, version: %08X, crc: %08X)",
               foundRes->fwName, amp.wmfwSize, fwVersion, fwCrc, foundRes->binName, amp.binSize, binVersion, binCrc);

    CIRRUS_LOG("tuning selection on %s: channel=%s spkid=%d bin=%s crc=0x%08X size=%lu (l0_crc=0x%08X r0_crc=0x%08X r0_present=%d)",
               amp.name, isRight ? "RIGHT(r0)" : "LEFT(l0)", spkid, (isRight && foundRes->binRight) ? "r0" : "l0", binCrc, chanBinSize,
               calculateCrc32(foundRes->bin, foundRes->binSize),
               foundRes->binRight ? calculateCrc32(foundRes->binRight, foundRes->binRightSize) : 0, foundRes->binRight ? 1 : 0);
    OSString* statusStr = OSString::withCString("READY");
    if (statusStr) {
        setProperty(propStatus, statusStr);
        statusStr->release();
    }
}

// Start the selected Halo image while keeping speaker output gated.
// Firmware control addresses come from the parsed image. Do not substitute
// addresses from another image merely because its algorithm names match.
bool CirrusAudioFixup::bringupDSP(AmplifierState& amp) {
    if (!amp.firmwareValidated || !amp.haloStateRegister || !amp.haloHeartbeatRegister)
        return false;
    auto failBoot = [&]() -> bool {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        if (!stopDSP(amp))
            amp.playbackFaulted = true;
        return false;
    };
    bool writesOk = true;
    auto checkedWrite = [&](uint32_t reg, uint32_t value) {
        if (writesOk)
            writesOk = writeRegister(amp, reg, value, TRACE_FIRMWARE);
    };
    char propName[64];
    OSString* statusStr = nullptr;

    uint32_t preCoreCtrl = 0, preClkCtrl = 0, preMbox = 0;
    uint32_t preSysId = 0, preSysVer = 0, preSysCore = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &preCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &preClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &preMbox))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &preSysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemVersion, &preSysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemCoreId, &preSysCore))
        return failBoot();
    CIRRUS_LOG("dsp pre-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X", amp.name,
               preCoreCtrl, preClkCtrl, preMbox, preSysId, preSysVer, preSysCore);

    static const uint32_t fsRateRegs[] = {
        cirrus::devices::cs35l41::registers::kRegDsp1Rx1Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx3Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx5Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx6Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx7Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx8Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx1Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx2Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx3Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx4Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx5Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx6Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx7Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx8Rate};
    bool fsErrataOk = true;
    for (size_t i = 0; i < sizeof(fsRateRegs) / sizeof(fsRateRegs[0]); ++i) {
        fsErrataOk &= writeRegister(amp, fsRateRegs[i], 0x00000001, TRACE_FIRMWARE);
    }
    CIRRUS_LOG("DSP FS errata on %s: %s", amp.name, fsErrataOk ? "applied" : "FAILED");
    if (!fsErrataOk)
        return failBoot();

    uint64_t mpuStart = mach_absolute_time();

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0x5555);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0xAAAA);

    uint32_t unlockVal = 0xFFFFFFFF;
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess0, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess1, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess2, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess3, unlockVal);

    bool mpuLocked = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0);
    if (!writesOk || !mpuLocked)
        return failBoot();

    uint64_t mpuEnd = mach_absolute_time();
    (void)mpuStart;
    (void)mpuEnd;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t currentClk = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &currentClk))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)currentClk, 32);

    uint64_t resetStart = mach_absolute_time();

    if (!updateRegisterBits(
            amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
            cirrus::devices::cs35l41::registers::kValHaloCoreReset | cirrus::devices::cs35l41::registers::kValHaloCoreEnable,
            cirrus::devices::cs35l41::registers::kValHaloCoreReset | cirrus::devices::cs35l41::registers::kValHaloCoreEnable) ||
        !updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
                            cirrus::devices::cs35l41::registers::kValHaloCoreReset, 0))
        return failBoot();

    uint64_t resetEnd = mach_absolute_time();
    uint64_t resetTimeUs = (resetEnd - resetStart) / 1000;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, resetTimeUs, 32);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t sysId = 0, sysVer = 0, sysCore = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemVersion, &sysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemCoreId, &sysCore))
        return failBoot();

    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_ID_%s", amp.name);
    setProperty(propName, (uint64_t)sysId, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_VER_%s", amp.name);
    setProperty(propName, (uint64_t)sysVer, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_CORE_%s", amp.name);
    setProperty(propName, (uint64_t)sysCore, 32);

    uint32_t postCoreCtrl = 0, postClkCtrl = 0, postMbox = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &postCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &postClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &postMbox))
        return failBoot();
    CIRRUS_LOG("dsp post-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X",
               amp.name, postCoreCtrl, postClkCtrl, postMbox, sysId, sysVer, sysCore);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_STATE_%s", amp.name);
    if (sysId == 0xFFFFFFFF && sysVer == 0xFFFFFFFF && sysCore == 0xFFFFFFFF) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_UNREACHABLE)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    } else if (sysId == 0x00000000 && sysVer == 0x00000000 && sysCore == 0x00000000) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_IN_RESET)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    }

    uint32_t mboxInit = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &mboxInit))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_RAW_INIT_%s", amp.name);
    setProperty(propName, (uint64_t)mboxInit, 32);

    const uint32_t HALO_STATE_REG = amp.haloStateRegister;
    const uint32_t HALO_STATE_CODE_RUN = 2;
    uint32_t haloState = 0;
    int haloTimeout = 15;
    while (haloTimeout > 0) {
        if (!readRegister(amp, HALO_STATE_REG, &haloState))
            return failBoot();
        if (haloState == HALO_STATE_CODE_RUN)
            break;
        IODelay(1000);
        haloTimeout--;
    }
    if (haloState == HALO_STATE_CODE_RUN) {
        CIRRUS_LOG("HALO firmware reached RUN state on %s (halo_state=0x%08X)", amp.name, haloState);

        if (!sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxPause,
                                cirrus::devices::cs35l41::registers::kStatusMailboxPaused))
            return failBoot();
    } else {
        CIRRUS_ERR("HALO firmware did NOT reach RUN on %s (halo_state=0x%08X, expected 0x2). "
                   "DSP boot likely failed — check wmfw/coeff upload PASS count above.",
                   amp.name, haloState);
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HALO_STATE_%s", amp.name);
    setProperty(propName, (uint64_t)haloState, 32);

    const uint32_t HALO_HEARTBEAT_REG = amp.haloHeartbeatRegister;
    uint32_t hb0 = 0, hb1 = 0, hb2 = 0;
    bool hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb0);
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb1) && hbReadable;
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb2) && hbReadable;
    const bool heartbeatMoving = (hb0 != hb1) && (hb1 != hb2);
    const char* hbVerdict;
    if (!hbReadable) {
        hbVerdict = "READ_FAILED";
    } else if (heartbeatMoving) {
        hbVerdict = "CHANGING";
    } else {
        hbVerdict = "UNCHANGED_WHILE_PAUSED_NOT_A_BOOT_VERDICT";
    }
    CIRRUS_LOG("dsp heartbeat on %s: t0=0x%08X t1=0x%08X t2=0x%08X -> %s", amp.name, hb0, hb1, hb2, hbVerdict);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_%s", amp.name);
    setProperty(propName, (uint64_t)hb2, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_VERDICT_%s", amp.name);
    statusStr = OSString::withCString(hbVerdict);
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    if (haloState != HALO_STATE_CODE_RUN) {
        uint32_t xmVioSts = 0, xmVioAddr = 0;
        uint32_t ymVioSts = 0, ymVioAddr = 0;
        uint32_t pmVioSts = 0, pmVioAddr = 0;
        if (!readRegister(amp, 0x02BC3104, &xmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3100, &xmVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC310C, &ymVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3108, &ymVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC3114, &pmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3110, &pmVioAddr))
            return failBoot();

        bool actualMpuFault = ((xmVioSts & 0x007E0000) != 0) || ((ymVioSts & 0x007E0000) != 0) || ((pmVioSts & 0x007E0000) != 0);

        CIRRUS_ERR("MPU FAULT REPORT on %s: XM vio_sts=0x%08X vio_addr=0x%08X | "
                   "YM vio_sts=0x%08X vio_addr=0x%08X | PM vio_sts=0x%08X vio_addr=0x%08X",
                   amp.name, xmVioSts, xmVioAddr, ymVioSts, ymVioAddr, pmVioSts, pmVioAddr);
        if (actualMpuFault) {
            CIRRUS_ERR("  -> MPU BLOCKED an access. A non-zero vio_sts (bits 17-22) means the core was "
                       "denied memory access (MPU still locked / wrong region). This is the fault cause.");
        } else {
            CIRRUS_ERR("  -> No MPU violation latched. Core stall is NOT an MPU block; "
                       "suspect firmware image integrity (addressing/byte-order) or missing errata.");
        }

        uint32_t scratch[4] = {0};
        if (!readRegister(amp, 0x02B805C0, &scratch[0]))
            return failBoot();
        if (!readRegister(amp, 0x02B805C8, &scratch[1]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D0, &scratch[2]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D8, &scratch[3]))
            return failBoot();
        CIRRUS_ERR("DSP SCRATCH on %s: s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X "
                   "(non-zero => firmware ROM wrote a panic/abort code)",
                   amp.name, scratch[0], scratch[1], scratch[2], scratch[3]);

        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_XM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)xmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_YM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)ymVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_PM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)pmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_SCRATCH1_%s", amp.name);
        setProperty(propName, (uint64_t)scratch[0], 32);
    }

    if (!hbReadable || haloState != HALO_STATE_CODE_RUN)
        return failBoot();
    CIRRUS_LOG("dsp bringup complete for %s", amp.name);
    return true;
}

// Check both core state and forward heartbeat progress.
// A readable mailbox alone can belong to a stalled DSP and is insufficient
// evidence for enabling speaker output.
bool CirrusAudioFixup::verifyDSPAlive(AmplifierState& amp) {
    uint32_t coreCtrl = 0, sysId = 0, mbox = 0, haloState = 0;
    bool corePass = false, resetPass = false, sysIdPass = false, mboxPass = false, xmPass = false;

    bool readable = amp.haloStateRegister && amp.haloHeartbeatRegister &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &coreCtrl, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sysId, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &mbox, TRACE_DUMP) &&
                    readRegister(amp, amp.haloStateRegister, &haloState, TRACE_DUMP);

    if (coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreEnable)
        corePass = true;
    if ((coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreReset) == 0)
        resetPass = true;
    if (sysId != 0x00000000 && sysId != 0xFFFFFFFF)
        sysIdPass = true;

    if (mbox == cirrus::devices::cs35l41::registers::kStatusMailboxPaused)
        mboxPass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xmPass = true;
    }

    uint32_t xmVio = 0, xmVioAddr = 0;
    uint32_t ymVio = 0, ymVioAddr = 0;
    uint32_t pmVio = 0, pmVioAddr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    readable = readRegister(amp, 0x2BC3104, &xmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ymVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ymVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP) && readable;

    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpuClean = ((xmVio & MPU_VIO_MASK) == 0 && (ymVio & MPU_VIO_MASK) == 0 && (pmVio & MPU_VIO_MASK) == 0);

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, sysid=%s(0x%08X), "
               "mailbox=%s(fw_status=%u), halo_state=0x%08X, xm_read=%s",
               amp.name, resetPass ? "ok" : "fail", corePass ? "ok" : "fail", sysIdPass ? "ok" : "fail", sysId, mboxPass ? "ok" : "fail",
               mbox, haloState, xmPass ? "ok" : "fail");
    CIRRUS_LOG("dsp verify MPU/scratch for %s: mpu=%s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X "
               "pm_vio=0x%08X@0x%08X scratch=[0x%08X 0x%08X 0x%08X 0x%08X]",
               amp.name, mpuClean ? "clean" : "FAULT", xmVio, xmVioAddr, ymVio, ymVioAddr, pmVio, pmVioAddr, sc1, sc2, sc3, sc4);

    uint32_t hb0 = 0, hb1 = 0;
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb0, TRACE_DUMP) && readable;
    IODelay(2000);
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb1, TRACE_DUMP) && readable;

    const uint32_t HALO_STATE_RUN = 2;
    bool runPass = (haloState == HALO_STATE_RUN);
    if (!runPass || !mpuClean || !mboxPass) {
        CIRRUS_ERR("dsp verdict for %s: halo=0x%08X heartbeat=0x%08X->0x%08X mpu=%s; DSP mode disabled", amp.name, haloState, hb0, hb1,
                   mpuClean ? "clean" : "FAULT");
    }

    return readable && corePass && resetPass && sysIdPass && mboxPass && xmPass && runPass && mpuClean;
}

bool CirrusAudioFixup::parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage) {
    uint8_t table[40 + 31 * 24] = {};
    outImage.algorithmCount = 0;
    if (!bulkRead(amp, 0x02800000, table, 40, TRACE_FIRMWARE))
        return false;
    uint32_t count = CirrusFirmwareParser::readUnpacked32BE(table, 9);
    if (!count || count > 31)
        return false;
    size_t length = 40 + count * 24;
    for (size_t offset = 40; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > 252)
            chunk = 252;
        if (!bulkRead(amp, static_cast<uint32_t>(0x02800000 + offset), table + offset, chunk, TRACE_FIRMWARE))
            return false;
        offset += chunk;
    }
    return CirrusFirmwareParser::parseAlgorithmTable(table, length, outImage);
}

void CirrusAudioFixup::uploadFirmware(AmplifierState& amp, const char* phaseArg) {
    (void)phaseArg;
    CIRRUS_LOG("starting firmware upload on %s", amp.name);

    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("no wmfw firmware data present on %s, skipping upload", amp.name);
        return;
    }

    FirmwareImage* image = (FirmwareImage*)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        return;
    }

    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    MappedImage* mappedImg = (MappedImage*)IOMalloc(sizeof(MappedImage));
    if (!mappedImg) {
        CIRRUS_ERR("failed to allocate memory for mapped image on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!CirrusFirmwareMapper::mapFirmwareImage(*image, *mappedImg)) {
        CIRRUS_ERR("firmware mapping failed on %s", amp.name);
        IOFree(mappedImg, sizeof(MappedImage));
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    UploadSession session;
    FixupRegisterIOAdapter io(this, amp);
    if (CirrusFirmwareScheduler::run(amp.name, io, *mappedImg, session)) {
        CIRRUS_LOG("firmware upload complete on %s", amp.name);
    } else {
        CIRRUS_ERR("firmware upload failed on %s", amp.name);
    }

    IOFree(mappedImg, sizeof(MappedImage));
    IOFree(image, sizeof(FirmwareImage));
}

bool CirrusAudioFixup::configureHardware(AmplifierState& amp) {
    CIRRUS_LOG("configuring generic serial port settings on %s", amp.name);
    bool ok = true;

    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask4, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask4, 0xFFFFFFFF) && ok;

    ok = writeRegister(amp, 0x00002020, 0x00000006) && ok;

    if (ok)
        CIRRUS_LOG("generic serial port config complete on %s", amp.name);
    return ok;
}

void CirrusAudioFixup::logASPSnapshot(AmplifierState& amp) {
    uint32_t enables = 0, rate = 0, fmt = 0, hiz = 0;
    uint32_t txWordLength = 0, rxWordLength = 0, txSlot = 0, rxSlot = 0;
    uint32_t rx1Src = 0, rx2Src = 0, rx3Src = 0, rx4Src = 0, rx5Src = 0;

    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &enables);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, &rate);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, &fmt);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, &hiz);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFrameTxSlot, &txSlot);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFrameRxSlot, &rxSlot);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortTxWordLength, &txWordLength);
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortRxWordLength, &rxWordLength);
    readRegister(amp, 0x00004C40, &rx1Src);
    readRegister(amp, 0x00004C44, &rx2Src);
    readRegister(amp, 0x00004C48, &rx3Src);
    readRegister(amp, 0x00004C4C, &rx4Src);
    readRegister(amp, 0x00004C50, &rx5Src);

    CIRRUS_LOG("asp snapshot for %s: enables=0x%08X rate=0x%08X format=0x%08X hiz=0x%08X", amp.name, enables, rate, fmt, hiz);
    CIRRUS_LOG("asp snapshot slots for %s: rx_slot=0x%08X tx_slot=0x%08X rx_wl=0x%08X tx_wl=0x%08X", amp.name, rxSlot, txSlot, rxWordLength,
               txWordLength);
    CIRRUS_LOG("asp snapshot routing for %s: rx1_src=0x%08X rx2_src=0x%08X rx3_src=0x%08X rx4_src=0x%08X rx5_src=0x%08X", amp.name, rx1Src,
               rx2Src, rx3Src, rx4Src, rx5Src);

    bool pass = true;
    uint32_t expectedRxSlot = (strcmp(amp.name, "right") == 0) ? 1 : 0;
    if ((rxSlot & 0x3F) != expectedRxSlot) {
        CIRRUS_ERR("asp rx slot mismatch on %s", amp.name);
        pass = false;
    }

    uint32_t expectedRx1Src = 0x08;
    uint32_t expectedRx2Src = (amp.monitorCount >= 1) ? 0x08 : 0x09;
    if (rx1Src != expectedRx1Src) {
        CIRRUS_ERR("asp rx1 source routing mismatch on %s", amp.name);
        pass = false;
    }
    if (rx2Src != expectedRx2Src) {
        CIRRUS_ERR("asp rx2 source routing mismatch on %s", amp.name);
        pass = false;
    }

    if (pass) {
        CIRRUS_LOG("asp validation check: pass on %s", amp.name);
    } else {
        CIRRUS_ERR("asp validation check: fail on %s", amp.name);
    }
}

void CirrusAudioFixup::logDSPSnapshot(AmplifierState& amp) {
    uint32_t dspState = 0, mbox1 = 0, mbox2 = 0;
    readRegister(amp, 0x00013004, &mbox2);
    readRegister(amp, 0x00013020, &mbox1);
    readRegister(amp, 0x02BC1000, &dspState);

    CIRRUS_LOG("dsp status snapshot for %s: state=0x%08X core=0x%08X mbox1=%u mbox2=%u", amp.name, mbox2, dspState, mbox1, mbox2);
}

void CirrusAudioFixup::logDSPBootReport(AmplifierState& amp) {
    uint32_t coreCtrl = 0, sysId = 0, haloState = 0;
    uint32_t mbox1 = 0, mbox2 = 0;
    uint32_t xmVio = 0, ymVio = 0, pmVio = 0;
    uint32_t xmVioAddr = 0, ymVioAddr = 0, pmVioAddr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    uint32_t ts0 = 0, ts1 = 0, ts2 = 0;

    bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &coreCtrl, TRACE_DUMP);
    readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sysId, TRACE_DUMP) && readable;
    readable = amp.haloStateRegister && readRegister(amp, amp.haloStateRegister, &haloState, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3104, &xmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ymVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ymVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pmVioAddr, TRACE_DUMP) && readable;
    readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP);
    readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP);
    readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP);
    readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP);

    bool hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts0, TRACE_DUMP);
    IODelay(2000);
    hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts1, TRACE_DUMP) && hbReadable;
    IODelay(2000);
    hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts2, TRACE_DUMP) && hbReadable;

    bool coreEn = (coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreEnable) != 0;
    bool coreOutOfReset = (coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreReset) == 0;
    bool sysIdOk = (sysId != 0x00000000 && sysId != 0xFFFFFFFF);
    bool runOk = (haloState == 2);
    bool mboxOk = (mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxRunning ||
                   mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxPaused);
    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpuClean = ((xmVio & MPU_VIO_MASK) == 0 && (ymVio & MPU_VIO_MASK) == 0 && (pmVio & MPU_VIO_MASK) == 0);
    bool hbAlive = (ts0 != ts1 && ts1 != ts2);

    const char* verdict;
    const char* rootCause;
    if (!readable) {
        verdict = "UNVERIFIED";
        rootCause = "required register read failed or control unresolved";
    } else if (!amp.firmwareValidated || !amp.dspAlive) {
        verdict = "DISABLED";
        rootCause = "initialization failed; current registers may reflect cleanup";
    } else if (coreEn && coreOutOfReset && sysIdOk && runOk && mpuClean && mboxOk) {
        verdict = "HEALTHY";
        rootCause = "HALO RUN and mailbox verified; heartbeat is diagnostic only";
    } else {
        verdict = "FAIL";
        rootCause = "required DSP state invariant failed; inspect raw registers";
    }

    CIRRUS_LOG("===== DSP BOOT REPORT for %s =====", amp.name);
    CIRRUS_LOG("  VERDICT   : %s", verdict);
    CIRRUS_LOG("  ROOT CAUSE: %s", rootCause);
    CIRRUS_LOG("  core      : ctrl=0x%08X en=%s reset_released=%s", coreCtrl, coreEn ? "yes" : "NO", coreOutOfReset ? "yes" : "NO");
    CIRRUS_LOG("  sys_id    : 0x%08X (%s)", sysId, sysIdOk ? "ok" : "BAD");
    CIRRUS_LOG("  halo_state: 0x%08X (%s, want RUN=2)", haloState, runOk ? "RUN" : "not-run");
    CIRRUS_LOG("  mailbox   : mbox1=0x%08X mbox2=0x%08X (%s)", mbox1, mbox2, mboxOk ? "ok" : "not-running");
    CIRRUS_LOG("  mpu       : %s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X pm_vio=0x%08X@0x%08X", mpuClean ? "clean" : "FAULT", xmVio,
               xmVioAddr, ymVio, ymVioAddr, pmVio, pmVioAddr);
    CIRRUS_LOG("  scratch   : [0x%08X 0x%08X 0x%08X 0x%08X] (firmware panic code if non-zero)", sc1, sc2, sc3, sc4);
    CIRRUS_LOG("  heartbeat : t0=0x%08X t1=0x%08X t2=0x%08X (%s)", ts0, ts1, ts2,
               !hbReadable ? "READ_FAILED"
               : hbAlive   ? "CHANGING"
                           : "UNCHANGED_NOT_A_BOOT_VERDICT");
    CIRRUS_LOG("===== END DSP BOOT REPORT for %s =====", amp.name);

    char propName[80];
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_VERDICT_%s", amp.name);
    OSString* vStr = OSString::withCString(verdict);
    if (vStr) {
        setProperty(propName, vStr);
        vStr->release();
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_ROOTCAUSE_%s", amp.name);
    OSString* rStr = OSString::withCString(rootCause);
    if (rStr) {
        setProperty(propName, rStr);
        rStr->release();
    }
}

void CirrusAudioFixup::snapshotPlayback(AmplifierState& amp) {
    uint32_t irq1Sts1 = 0, irq1Sts2 = 0, irq1Sts3 = 0, irq1Sts4 = 0;
    uint32_t pwrMgtSts = 0;

    readRegister(amp, 0x00010010, &irq1Sts1);
    readRegister(amp, 0x00010014, &irq1Sts2);
    readRegister(amp, 0x00010018, &irq1Sts3);
    readRegister(amp, 0x0001001C, &irq1Sts4);
    readRegister(amp, 0x00002908, &pwrMgtSts);

    CIRRUS_LOG("playback interrupts snapshot for %s: irq1=0x%08X irq2=0x%08X irq3=0x%08X irq4=0x%08X power_status=0x%08X", amp.name,
               irq1Sts1, irq1Sts2, irq1Sts3, irq1Sts4, pwrMgtSts);
}

void CirrusAudioFixup::snapshotDiagnostics(AmplifierState& amp, const char* stage) {
    // This sweep feeds routine logs only. Essential power, DSP and protection
    // checks run at their call sites in either build. Avoid delaying the next
    // stereo endpoint with dozens of I2C reads when nobody requested verbosity.
    if (!gCirrusDebug)
        return;
    uint32_t irq1[4] = {0}, irq1Mask[4] = {0};
    uint32_t irq2[4] = {0}, irq2Mask[4] = {0};
    uint32_t spEn = 0, spRate = 0, spFmt = 0, spHiz = 0;
    uint32_t pwrSts = 0, strmErr = 0;

    CIRRUS_LOG("diagnostics dump (%s) for amplifier %s:", stage, amp.name);

    readRegister(amp, 0x00010010, &irq1[0]);
    readRegister(amp, 0x00010014, &irq1[1]);
    readRegister(amp, 0x00010018, &irq1[2]);
    readRegister(amp, 0x0001001C, &irq1[3]);

    uint32_t rawSts3 = 0;
    readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, &rawSts3);
    bool pupDone = (irq1[0] & 0x01000000) != 0;
    bool ampShort = (irq1[0] & 0x80000000) != 0;
    bool dspError = (irq1[0] & 0x00000002) != 0;
    bool pllLock = (rawSts3 & 0x00000002) != 0;

    CIRRUS_LOG("decoded interrupts on %s: pup_done=%d amp_short=%d dsp_err=%d pll_lock=%d", amp.name, pupDone, ampShort, dspError, pllLock);
    CIRRUS_LOG("irq1 status values on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X raw3=0x%08X", amp.name, irq1[0], irq1[1], irq1[2], irq1[3],
               rawSts3);

    readRegister(amp, 0x00010110, &irq1Mask[0]);
    readRegister(amp, 0x00010114, &irq1Mask[1]);
    readRegister(amp, 0x00010118, &irq1Mask[2]);
    readRegister(amp, 0x0001011C, &irq1Mask[3]);
    CIRRUS_LOG("irq1 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq1Mask[0], irq1Mask[1], irq1Mask[2],
               irq1Mask[3]);

    readRegister(amp, 0x00010810, &irq2[0]);
    readRegister(amp, 0x00010814, &irq2[1]);
    readRegister(amp, 0x00010818, &irq2[2]);
    readRegister(amp, 0x0001081C, &irq2[3]);
    CIRRUS_LOG("irq2 status registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2[0], irq2[1], irq2[2], irq2[3]);

    readRegister(amp, 0x00010910, &irq2Mask[0]);
    readRegister(amp, 0x00010914, &irq2Mask[1]);
    readRegister(amp, 0x00010918, &irq2Mask[2]);
    readRegister(amp, 0x0001091C, &irq2Mask[3]);
    CIRRUS_LOG("irq2 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2Mask[0], irq2Mask[1], irq2Mask[2],
               irq2Mask[3]);

    readRegister(amp, 0x00004800, &spEn);
    readRegister(amp, 0x00004804, &spRate);
    readRegister(amp, 0x00004808, &spFmt);
    readRegister(amp, 0x0000480C, &spHiz);
    CIRRUS_LOG("serial port configuration for %s: enables=0x%08X rate=0x%08X", amp.name, spEn, spRate);
    CIRRUS_LOG("serial port format for %s: format=0x%08X hiz=0x%08X", amp.name, spFmt, spHiz);

    readRegister(amp, 0x00002908, &pwrSts);
    readRegister(amp, 0x02BC5A08, &strmErr);
    CIRRUS_LOG("power status on %s: power_mgt=0x%08X arb_error=0x%08X", amp.name, pwrSts, strmErr);

    uint32_t dspTs = 0, mdsyncRx = 0;
    readRegister(amp, 0x025C0800, &dspTs);
    readRegister(amp, 0x00003420, &mdsyncRx);
    CIRRUS_LOG("dsp execution registers on %s: timestamp=0x%08X mdsync=0x%08X", amp.name, dspTs, mdsyncRx);

    uint32_t dspMbox1 = 0, dspMbox2 = 0;
    readRegister(amp, 0x00013020, &dspMbox1);
    readRegister(amp, 0x00013004, &dspMbox2);
    CIRRUS_LOG("mailbox states on %s: mbox1=0x%08X mbox2=0x%08X", amp.name, dspMbox1, dspMbox2);

    for (uint32_t i = 0; i < amp.diagnosticControlCount; i++) {
        uint32_t val = 0;
        bool valid = readRegister(amp, amp.diagnosticControls[i].address, &val);
        CIRRUS_LOG("control register on %s: %s (0x%08X) valid=%d value=0x%08X", amp.name, amp.diagnosticControls[i].name,
                   amp.diagnosticControls[i].address, valid, val);
    }
}

void CirrusAudioFixup::logPowerSnapshot(AmplifierState& amp) {
    uint32_t pwrCtrl1 = 0, pwrCtrl2 = 0, pwrCtrl3 = 0;
    readRegister(amp, 0x00002014, &pwrCtrl1);
    readRegister(amp, 0x00002018, &pwrCtrl2);
    readRegister(amp, 0x0000201C, &pwrCtrl3);

    CIRRUS_LOG("power rails snapshot for %s: ctrl1=0x%08X ctrl2=0x%08X ctrl3=0x%08X", amp.name, pwrCtrl1, pwrCtrl2, pwrCtrl3);

    bool pass = true;

    if ((pwrCtrl1 & 1) != 0) {
        CIRRUS_ERR("global enable unexpectedly active while idle on %s", amp.name);
        pass = false;
    }
    if ((pwrCtrl2 & 1) != 0) {
        CIRRUS_ERR("amplifier stage unexpectedly enabled while idle on %s", amp.name);
        pass = false;
    }

    if (pass) {
        CIRRUS_LOG("idle power verify results: pass on %s (GLOBAL_EN safely off)", amp.name);
    } else {
        CIRRUS_ERR("power verify results: fail on %s", amp.name);
    }
}

// Prepare the profile's output path in muted idle.
// This step is not permission to play. The monitor separately checks HDA,
// PLL, DSP and power acknowledgements before applying operating gain.
bool CirrusAudioFixup::powerUpAmplifier(AmplifierState& amp) {
    CIRRUS_LOG("starting power up sequence for %s (DSP Mode=%d)", amp.name, amp.monitorCount);
    bool ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0) && ok;
    if (!ok || !checkProtectionStatus(amp)) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
        return false;
    }

    if (amp.monitorCount == 1) {
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010001) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000003) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x00000032) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000028) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000029) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000029) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx6Source, 0x00000029) && ok;
    } else {
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010000) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000002) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x08) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000032) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000033) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000009) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000020) && ok;
    }

    ok = updateRegisterBits(amp, 0x00002018, 0x00000001, 0x00000000) && ok;

    ok = writeRegister(amp, 0x00000040, 0x00000055) && ok;
    ok = writeRegister(amp, 0x00000040, 0x000000AA) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegPowerControl1, 1, 0) && ok;
    ok = writeRegister(amp, 0x00007438, 0x00585941) && ok;
    ok = writeRegister(amp, 0x00007414, 0x08C82222) && ok;
    ok = writeRegister(amp, 0x0000742C, 0x00000009) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegPowerControl2,
                            0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 0) &&
         ok;
    ok = writeRegister(amp, 0x00000040, 0x000000CC) && ok;
    ok = writeRegister(amp, 0x00000040, 0x00000033) && ok;

    uint32_t digVol = 0, gainCtrl = 0;
    ok = readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol) && ok;
    ok = readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gainCtrl) && ok;
    CIRRUS_LOG("default volume states on %s: digital_volume=0x%08X gain=0x%08X", amp.name, digVol, gainCtrl);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0x00000000) && ok;

    if (!ok) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
    }
    logPowerSnapshot(amp);
    snapshotDiagnostics(amp, "IDLE (POST-BOOT)");
    return ok;
}

// Compare the final idle registers with the mode-specific expectations.
// Failed readback requires rollback; do not mark an amplifier initialized
// just because its configuration writes completed.
bool CirrusAudioFixup::verifyIdleConfiguration(AmplifierState& amp) {
    uint32_t pwr1 = 0, pwr2 = 0, spEn = 0, spFmt = 0;
    uint32_t dacSrc = 0, digVol = 0, gain = 0;
    bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegPowerControl1, &pwr1, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegPowerControl2, &pwr2, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &spEn, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, &spFmt, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, &dacSrc, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PROBE);
    bool dspMode = amp.monitorCount >= 1;
    uint32_t expectedSpEn = dspMode ? 0x00010001U : 0x00010000U;
    uint32_t expectedDac = dspMode ? 0x00000032U : 0x00000008U;
    bool valid = readable && ((pwr1 & 0x1) == 0) && ((pwr2 & (0x00003001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == 0) &&
                 spEn == expectedSpEn && spFmt == 0x20200200U && dacSrc == expectedDac && digVol == 0x0000A678U && gain == 0;
    if (!valid) {
        CIRRUS_ERR("idle verify %s: read=%d pwr1=0x%08X pwr2=0x%08X sp_en=0x%08X sp_fmt=0x%08X dac=0x%08X vol=0x%08X gain=0x%08X mode=%s",
                   amp.name, readable, pwr1, pwr2, spEn, spFmt, dacSrc, digVol, gain, dspMode ? "DSP" : "BYPASS");
    }
    return valid;
}
