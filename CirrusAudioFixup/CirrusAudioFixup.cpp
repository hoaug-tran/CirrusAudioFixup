#include "CirrusAudioFixup.hpp"

#include "Devices/CS35L41/Resources/FirmwareDatabase.hpp"
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

public:
    FixupRegisterIOAdapter(CirrusAudioFixup* fixup, AmplifierState& amp) : mFixup(fixup), mAmp(amp) {}

    bool read(uint32_t address, uint32_t* value) override {
        return mFixup->readRegister(mAmp, address, value, cirrus::diagnostics::TRACE_PROBE);
    }

    bool write(uint32_t address, uint32_t value) override {
        return mFixup->writeRegister(mAmp, address, value, cirrus::diagnostics::TRACE_PROBE);
    }

    bool updateBits(uint32_t address, uint32_t mask, uint32_t value) override {
        return mFixup->updateRegisterBits(mAmp, address, mask, value, cirrus::diagnostics::TRACE_PROBE);
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
    gCirrusDebug = bootArgEnabled("-cirrusdbg");
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

bool CirrusAudioFixup::start(IOService* provider) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = bootArgEnabled("-cirrusdbg");

    IOLog("============================================================\n");
    IOLog("  CirrusAudioFixup by Tran Kinh Hoang (hoaugtr)\n");
    IOLog("  Smart Amplifier Fixup for macOS\n");
    IOLog("============================================================\n");

    setProperty("Author", "Tran Kinh Hoang (hoaugtr)");
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("start");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
        return false;
    }

    mProvider = provider;
    logProviderInfo(provider);
    dumpProviderProperties(provider);

    if (bootArgEnabled("-cirrusro") || bootArgEnabled("-cirrusro")) {
        if (!setupProbeTimer()) {
            mProvider = nullptr;
            super::stop(provider);
            return false;
        }
        uint32_t delayMs = 100;
        PE_parse_boot_argn("-cirrusdelay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
        if (!setupPowerManagement(provider)) {
            stop(provider);
            return false;
        }
        registerService();
        return true;
    }

    IOMemoryDescriptor* bmd = nullptr;
    IOService* gpioOwner = nullptr;
    bool hardwareResetVerified = false;
    OSDictionary* dict = IOService::nameMatching("AMDI0030");
    if (dict) {
        OSIterator* iter = IOService::getMatchingServices(dict);
        if (iter) {
            IOService* amdi0030 = OSDynamicCast(IOService, iter->getNextObject());
            if (amdi0030) {
                if (amdi0030->open(this)) {
                    amdi0030->retain();
                    gpioOwner = amdi0030;
                    IOMemoryDescriptor* bmd0 = amdi0030->getDeviceMemoryWithIndex(0);
                    if (bmd0 && bmd0->getLength() >= 0x400) {
                        CIRRUS_LOG("Found AMDI0030 base physical address: 0x%llX", (unsigned long long)bmd0->getPhysicalAddress());
                        bmd0->retain();
                        bmd = bmd0;
                    }
                } else {
                    CIRRUS_ERR("Failed to open AMDI0030");
                }
            }
            iter->release();
        }
        dict->release();
    }
    if (bmd) {
        if (bmd->prepare() == kIOReturnSuccess) {
            IOMemoryMap* map = bmd->map();
            if (map) {
                volatile UInt32* gpioBase = (volatile UInt32*)map->getVirtualAddress();

                UInt32 val = gpioBase[6];
                setProperty("Cirrus_GPIO6_old", val, 32);
                CIRRUS_LOG("AMD GPIO 6 old value: 0x%08X", val);

                val &= ~(1 << 22);
                val |= (1 << 23);
                gpioBase[6] = val;

                UInt32 verifyLow = gpioBase[6];
                setProperty("Cirrus_GPIO6_verifyLow", verifyLow, 32);
                CIRRUS_LOG("AMD GPIO 6 LOW verify = 0x%08X", verifyLow);

                IOSleep(5);

                val = gpioBase[6];
                val |= (1 << 22);
                val |= (1 << 23);
                gpioBase[6] = val;

                UInt32 verifyHigh = gpioBase[6];
                setProperty("Cirrus_GPIO6_verifyHigh", verifyHigh, 32);
                CIRRUS_LOG("AMD GPIO 6 HIGH verify = 0x%08X", verifyHigh);
                hardwareResetVerified = ((verifyLow & (1U << 22)) == 0) && ((verifyHigh & (1U << 22)) != 0);

                map->release();
            }
            bmd->complete();
        }
        bmd->release();
    }
    if (gpioOwner) {
        gpioOwner->close(this);
        gpioOwner->release();
    }

    if (!hardwareResetVerified) {
        CIRRUS_ERR("DIAG_FAIL reset GPIO6 could not be toggled and verified");
        for (unsigned i = 0; i < 2; ++i) {
            setDiagnosticStage(mAmps[i], STAGE_RESET);
            recordDiagnosticFailure(mAmps[i], DIAG_RESET_GPIO, 6, 1, 0, kIOReturnNotReady, false);
        }
    }

    IOSleep(15);

    if (!setupProbeTimer()) {
        CIRRUS_ERR("probe timer setup failed");
        mProvider = nullptr;
        super::stop(provider);
        return false;
    }

    bool probeEnabled = bootArgEnabled("-cirrusprobe");
    setProperty("CirrusBootArgParsed", probeEnabled ? kOSBooleanTrue : kOSBooleanFalse);

    if (bootArgEnabled("-cirrusro") || bootArgEnabled("-cirrusro")) {
        CIRRUS_LOG("CirrusAudioFixup starting in READ-ONLY PROBE mode");
        uint32_t delayMs = 100;
        PE_parse_boot_argn("-cirrusdelay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
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

void CirrusAudioFixup::fullDriverFlow() {
    CIRRUS_LOG("starting hardware initialization flow");
    const bool bypass = bootArgEnabled("-cirrusnodsp") || bootArgEnabled("-cirrusnodsp");

    for (unsigned i = 0; i < 2; ++i) {
        AmplifierState& amp = mAmps[i];
        if (amp.playbackFaulted)
            continue;
        CIRRUS_LOG("initializing amplifier: %s", amp.name);

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
                recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId, cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_OTP_BOOT);

        setDiagnosticStage(amp, STAGE_ERRATA);
        if (!initializeHardwareErrata(amp)) {
            CIRRUS_ERR("failed to apply hardware errata for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_ERRATA);

        setDiagnosticStage(amp, STAGE_CLOCK);
        if (!applyPLL(amp)) {
            recordDiagnosticFailure(amp, DIAG_PLL_CONFIG, cirrus::devices::cs35l41::registers::kRegPllClockControl);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_CLOCK);

        setDiagnosticStage(amp, STAGE_ASP);
        if (!applyASP(amp)) {
            recordDiagnosticFailure(amp, DIAG_ASP_CONFIG, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_ASP);

        setDiagnosticStage(amp, STAGE_GPIO);
        if (!applyGPIO(amp)) {
            recordDiagnosticFailure(amp, DIAG_GPIO_CONFIG, cirrus::devices::cs35l41::registers::kRegGpioPadControl, 0x02000000);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_GPIO);

        setDiagnosticStage(amp, STAGE_PLATFORM);
        if (!configureHardware(amp)) {
            CIRRUS_ERR("platform hardware configuration failed for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_PLATFORM_CONFIG);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_PLATFORM);

        if (bypass) {
            if (!stopDSP(amp)) {
                recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
                abortInitialization();
                continue;
            }
        } else {
            setDiagnosticStage(amp, STAGE_FIRMWARE_DISCOVERY);
            discoverFirmware(amp);
            if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
                recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
                abortInitialization();
                continue;
            }
            markDiagnosticSuccess(amp, STAGE_FIRMWARE_DISCOVERY);
            setDiagnosticStage(amp, STAGE_FIRMWARE_UPLOAD);
            initializeFirmware(amp, "5D.0");
            if (amp.playbackFaulted || !amp.firmwareValidated || !amp.dspAlive || amp.monitorCount != 1) {
                if (amp.diagnostic.latestFailure == DIAG_OK)
                    recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
                abortInitialization();
                continue;
            }
            markDiagnosticSuccess(amp, STAGE_DSP_BOOT);
        }

        setDiagnosticStage(amp, STAGE_IDLE_VERIFY);
        if (!powerUpAmplifier(amp)) {
            recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
            abortInitialization();
            continue;
        }
        if (!verifyIdleConfiguration(amp)) {
            CIRRUS_ERR("idle hardware verification failed for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_IDLE_INVARIANT);
            abortInitialization();
            continue;
        }
        if (!bypass && !verifyDSPAlive(amp)) {
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
        logASPSnapshot(amp);

        amp.initialized = true;

        CIRRUS_LOG("amplifier %s initialized successfully (dspAlive=%d)", amp.name, amp.dspAlive);
    }

    publishDriverVerdict();
    CIRRUS_LOG("hardware initialization flow complete");
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

void CirrusAudioFixup::stop(IOService* provider) {
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
    if (mTraceLock) {
        IOLockFree(mTraceLock);
        mTraceLock = nullptr;
    }
    super::free();
}

bool CirrusAudioFixup::bootArgEnabled(const char* name) {
    UInt32 value = 0;
    if (PE_parse_boot_argn(name, &value, sizeof(value))) {
        return value != 0;
    }

    int strValue[16];
    if (PE_parse_boot_argn(name, &strValue, sizeof(strValue))) {
        char* strPtr = reinterpret_cast<char*>(&strValue);
        if (strPtr[0] == '0' && strPtr[1] == '\0') {
            return false;
        }
        return true;
    }

    return false;
}

bool CirrusAudioFixup::bootArgStrEquals(const char* name, const char* expectedVal) {
    char val[64];
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

void CirrusAudioFixup::initializeFirmware(AmplifierState& amp, const char* phaseArg) {
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
        size_t str_len = strlen(str);
        size_t sub_len = strlen(sub);
        if (sub_len > str_len)
            return false;
        for (size_t i = 0; i <= str_len - sub_len; i++) {
            bool match = true;
            for (size_t j = 0; j < sub_len; j++) {
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

bool CirrusAudioFixup::applyCalibration(AmplifierState& amp, const FirmwareImage* image) {
    const char* fields[] = {"Status", "R0", "Ambient", "Valid", "Checksum"};
    for (const char* field : fields) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_%s_%s", field, amp.name);
        removeProperty(property);
    }
    if (!image)
        return false;

    if (bootArgEnabled("-cirrusnocal") || bootArgEnabled("-cirrusnocal")) {
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
                if (count > ampIdx && count <= (len - sizeof(cs35l41_amp_efi_data)) / sizeof(cs35l41_amp_cal_data)) {
                    const cs35l41_amp_cal_data& cl = efiData->data[ampIdx];
                    ambientVal = cl.calAmbient;
                    statusVal = cl.calStatus;
                    r0Val = OSSwapLittleToHostInt16(cl.calR);
                    checksumVal = r0Val + 1;
                    foundData = true;
                    invalidData = false;
                    sourceStr = "APPLIED_FROM_EFI_VARIABLE";
                }
            }
        }
        if (prop)
            prop->release();
        options->release();
    }
    if (invalidData) {
        publishStatus("INVALID_EFI_DATA");
        return false;
    }

    if (!foundData) {
        uint32_t bootR0 = 0;
        const char* r0Arg = (ampIdx == 0) ? "cirrus_cal_r0_l" : "cirrus_cal_r0_r";
        if (PE_parse_boot_argn("-cirruscalr0", &bootR0, sizeof(bootR0))) {
            r0Val = bootR0;
            statusVal = 1;
            ambientVal = 0;
            PE_parse_boot_argn("-cirruscalstatus", &statusVal, sizeof(statusVal));
            if (!PE_parse_boot_argn("-cirruscalambient", &ambientVal, sizeof(ambientVal)) || !r0Val || r0Val > 0xFFFFU ||
                statusVal > 0xFFU || ambientVal < -128 || ambientVal > 127) {
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

void CirrusAudioFixup::runReadOnlyProbe() {
    CIRRUS_LOG("read-only probe begin");

    for (unsigned i = 0; i < 2; ++i) {
        probeAmp(mAmps[i]);
    }

    publishStatistics();

    if (bootArgEnabled("-cirrusdumptrace")) {
        dumpTraceBuffer();
    }

    CIRRUS_LOG("read-only probe complete");
}

bool CirrusAudioFixup::supportedHdaFormat(uint16_t format) {
    const uint32_t baseRate = (format & 0x4000) ? 44100 : 48000;
    const uint32_t multiplier = ((format >> 11) & 7) + 1;
    const uint32_t divisor = ((format >> 8) & 7) + 1;
    const uint32_t sampleSize = (format >> 4) & 7;

    return !(format & 0x8080) && multiplier <= 4 && baseRate * multiplier == 48000 * divisor && (format & 15) == 1 && sampleSize >= 1 &&
           sampleSize <= 3;
}

bool CirrusAudioFixup::syncAlc287HdaCodec() {
    const bool wasPrepared = mHdaState.converterPrepared;
    auto publishStatus = [&](const char* text) {
        OSString* status = OSString::withCString(text);
        if (status) {
            setProperty("Cirrus_HDA_Status", status);
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
    IOService* audioCtrl = getAudioController();
    if (!audioCtrl) {
        if (mHdaState.missCount < 20 && ++mHdaState.missCount == 20) {
            for (unsigned i = 0; i < 2; ++i) {
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

    IOMemoryMap* map = pciDev->mapDeviceMemoryWithRegister(0x10);
    if (!map || map->getLength() < 0x80) {
        publishStatus("INVALID_BAR_MAPPING");
        if (map)
            map->release();
        audioCtrl->release();
        return false;
    }
    volatile uint8_t* base = (volatile uint8_t*)map->getVirtualAddress();
    mHdaState.observed = base != nullptr;
    if (!base)
        publishStatus("INVALID_BAR_ADDRESS");

    bool outputRunning = false;
    uint8_t activeStream = 0;
    uint16_t activeFormat = 0;
    uint8_t activeDescriptor = 0xFF;
    unsigned runningOutputs = 0;
    if (base) {
        uint16_t gcap = *(volatile uint16_t*)(base + 0x00);
        uint32_t gctl = *(volatile uint32_t*)(base + 0x08);
        if (gcap == 0xFFFF || gctl == 0xFFFFFFFF || !(gctl & 1)) {
            publishStatus("CONTROLLER_NOT_READY");
            mHdaState.observed = false;
            map->release();
            audioCtrl->release();
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
            map->release();
            audioCtrl->release();
            return false;
        }

        if (!mHdaState.topologyLogged) {
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
    }
    if (runningOutputs > 1) {
        publishStatus("AMBIGUOUS_OUTPUT_STREAMS");
        map->release();
        audioCtrl->release();
        return false;
    }
    bool converterPrepared = outputRunning && activeStream != 0 && supportedHdaFormat(activeFormat);
    bool streamActive = converterPrepared;

    if (outputRunning && activeStream != 0 && !supportedHdaFormat(activeFormat)) {
        for (unsigned i = 0; i < 2; ++i) {
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
    if (base)
        publishStatus(converterPrepared ? "RUNNING_ROUTE_UNCONFIRMED" : outputRunning ? "UNSUPPORTED_STREAM_FORMAT" : "IDLE");
    mHdaState.converterPrepared = converterPrepared;
    mHdaState.streamActive = streamActive;
    mHdaState.lastDescriptor = activeDescriptor;
    mHdaState.lastStreamTag = activeStream;
    mHdaState.lastFormat = activeFormat;
    map->release();
    audioCtrl->release();
    return streamActive;
}

bool CirrusAudioFixup::checkProtectionStatus(AmplifierState& amp) {
    uint32_t status = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kIrq1Status1Register, &status, TRACE_PLAYBACK))
        return false;
    if (status & cirrus::devices::cs35l41::registers::kMaskProtectionFault) {
        recordDiagnosticFailure(amp, DIAG_AMP_PROTECTION, cirrus::devices::cs35l41::registers::kIrq1Status1Register, 0, status);
        return false;
    }
    return true;
}

bool CirrusAudioFixup::stopPlayback(AmplifierState& amp) {
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
        IODelay(1000);
    }
    if (!pdnDone)
        recordDiagnosticFailure(amp, DIAG_POWER_DOWN_TIMEOUT, 0x00010010, 0x00800000, irq);
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00580941, TRACE_PLAYBACK) && ok;
    ok = lockTestKey(amp) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegGpio1Control1, 0x00000001, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 1, 0, TRACE_PLAYBACK) && ok;
    if (amp.dspAlive) {
        bool paused = sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxPause,
                                         cirrus::devices::cs35l41::registers::kStatusMailboxPaused);
        ok = paused && ok;
        if (!paused) {
            amp.firmwareValidated = false;
            stopDSP(amp);
        }
    }
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 0,
                            TRACE_PLAYBACK) &&
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

void CirrusAudioFixup::runBackgroundMonitor() {
    if (mStopping || !mPowerAvailable || mNeedsReinitialization)
        return;
    bool hdaStreamActive = syncAlc287HdaCodec();
    IOSleep(5);
    for (unsigned i = 0; i < 2; ++i) {
        AmplifierState& amp = mAmps[i];
        if (!amp.present)
            continue;
        if (amp.playbackFaulted) {
            if (amp.playbackActive && amp.cleanupAttempts < 3)
                stopPlayback(amp);
            continue;
        }

        if (amp.initialized && !amp.playbackActive && mHdaState.observed && !hdaStreamActive) {
            amp.playbackStableCount = 0;
            continue;
        }

        uint32_t pwr_ctrl1 = 0;
        uint32_t pll_lock_sts = 0;
        uint32_t mbox2 = 0;
        uint32_t timestamp = 0;

        if (!readRegister(amp, 0x00002014, &pwr_ctrl1, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        bool pllReadable = readRegister(amp, 0x00010098, &pll_lock_sts, TRACE_DUMP);
        if (!pllReadable) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }
        bool global_en = (pwr_ctrl1 & 0x01) != 0;
        bool pll_lock = pllReadable && (pll_lock_sts & 0x00000002) != 0;

        if (!readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!amp.initialized) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!checkProtectionStatus(amp)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!readRegister(amp, 0x025C0800, &timestamp, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        bool hasAudio = mHdaState.observed && hdaStreamActive;

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
            if (!global_en || !pll_lock || !powerReadable ||
                (pwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) != (dspMode ? 0x3001U : 1U) || !dspHealthy) {
                CIRRUS_ERR("ACTIVE_STATE_LOST amp=%s pwr1=0x%08X pwr2=0x%08X pll=%d mbox=%u core=0x%08X halo=%u", amp.name, pwr_ctrl1, pwr2,
                           pll_lock, mbox2, core, halo);
                if (!pll_lock)
                    recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, 2,
                                            pll_lock_sts);
                else
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::cs35l41::registers::kPowerControl2, dspMode ? 0x3001U : 1U, pwr2);
                amp.playbackFaulted = true;
                stopPlayback(amp);
                continue;
            }
        }

        bool playbackTransition = hasAudio != amp.playbackActive;
        bool heartbeat = (++amp.monitorLogCountdown >= (hasAudio ? 50U : 40U));
        if (playbackTransition || heartbeat) {
            uint32_t asp_enables = 0, sp_rate = 0, sp_fmt = 0, dac_src = 0, amp_vol = 0;
            bool snapshotReadable =
                readRegister(amp, 0x00004800, &asp_enables, TRACE_DUMP) && readRegister(amp, 0x00004804, &sp_rate, TRACE_DUMP) &&
                readRegister(amp, 0x00004808, &sp_fmt, TRACE_DUMP) && readRegister(amp, 0x00004C00, &dac_src, TRACE_DUMP) &&
                readRegister(amp, 0x00006000, &amp_vol, TRACE_DUMP);
            if (!snapshotReadable) {
                amp.playbackFaulted = true;
                stopPlayback(amp);
                continue;
            }

            CIRRUS_LOG("background monitor %s: global_en=%d pll_lock=%d mbox2=0x%08X ts=0x%08X->0x%08X audio=%d active=%d "
                       "hda_sd=%u tag=%u fmt=0x%04X sp_en=0x%08X sp_rate=0x%08X sp_fmt=0x%08X dac_src=0x%08X vol=0x%08X mode=%s",
                       amp.name, global_en, pll_lock, mbox2, amp.lastTimestamp, timestamp, hasAudio, amp.playbackActive, mHdaState.lastDescriptor,
                       mHdaState.lastStreamTag, mHdaState.lastFormat, asp_enables, sp_rate, sp_fmt, dac_src, amp_vol,
                       (amp.monitorCount >= 1) ? "DSP" : "BYPASS");
            amp.monitorLogCountdown = 0;
        }

        amp.lastTimestamp = timestamp;

        if (hasAudio && !amp.playbackActive) {
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 1) {
                const uint32_t startStreamTag = mHdaState.lastStreamTag;
                const uint32_t startStreamFormat = mHdaState.lastFormat;
                bool dspMode = (amp.monitorCount >= 1);
                if ((!dspMode && !bootArgEnabled("-cirrusnodsp")) || (dspMode && (!amp.dspAlive || !amp.firmwareValidated))) {
                    amp.playbackFaulted = true;
                    stopPlayback(amp);
                    continue;
                }
                bool dspCommandOk = true;
                bool sequenceOk = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678);
                sequenceOk = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0) && sequenceOk;
                auto checkedWrite = [&](uint32_t reg, uint32_t value) {
                    if (sequenceOk)
                        sequenceOk = writeRegister(amp, reg, value, TRACE_PLAYBACK);
                };
                auto abortStart = [&]() {
                    amp.playbackFaulted = true;
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT);
                    stopPlayback(amp);
                };
                setDiagnosticStage(amp, STAGE_PLAYBACK_OPEN);
                CIRRUS_LOG("Background Monitor: Playback STARTED on %s (mode=%s) — enabling output path", amp.name,
                           dspMode ? "DSP" : "BYPASS");
                amp.playbackStableCount = 0;

                if (dspMode) {
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegPllClockControl, 0x00000430);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDspClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegGlobalClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010001);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, 0x00000021);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortTxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x00000032);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx1Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx2Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000028);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000029);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000029);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx6Source, 0x00000029);
                    sequenceOk = sequenceOk && updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 0x00003000, 0x00003000);
                    dspCommandOk = sequenceOk && sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxResume,
                                                                    cirrus::devices::cs35l41::registers::kStatusMailboxRunning);
                    sequenceOk = sequenceOk && dspCommandOk;
                } else {
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegPllClockControl, 0x00000430);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDspClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegGlobalClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010000);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, 0x00000021);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000002);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortTxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx1Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx2Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000032);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000033);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000009);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000020);
                }

                if (!sequenceOk ||
                    !updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 1 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 1)) {
                    abortStart();
                    continue;
                }

                checkedWrite(cirrus::devices::cs35l41::registers::kRegGpio1Control1, 0x00008001);
                checkedWrite(0x00010010, 0x01000000);

                setDiagnosticStage(amp, STAGE_PLAYBACK_PREPARE);
                checkedWrite(0x00000040, 0x00000055);
                checkedWrite(0x00000040, 0x000000AA);
                checkedWrite(0x0000742C, 0x0000000F);
                checkedWrite(0x0000742C, 0x00000079);
                checkedWrite(0x00007438, 0x00585941);

                sequenceOk = sequenceOk && updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl1, 1, 1);
                if (!sequenceOk) {
                    abortStart();
                    continue;
                }

                uint32_t irq1_sts = 0;
                int pup_timeout = 100;
                int elapsed_pup = 0;
                while (pup_timeout > 0) {
                    if (!readRegister(amp, 0x00010010, &irq1_sts)) {
                        sequenceOk = false;
                        break;
                    }
                    if (irq1_sts & 0x01000000)
                        break;
                    IODelay(1000);
                    elapsed_pup++;
                    pup_timeout--;
                }
                bool pupDone = (irq1_sts & 0x01000000) != 0;
                if (pupDone) {
                    CIRRUS_LOG("Background Monitor: PUP_DONE observed on %s after %d ms", amp.name, elapsed_pup);
                    checkedWrite(0x00010010, 0x01000000);
                } else {
                    CIRRUS_ERR("Background Monitor: PUP_DONE NOT observed on %s within 100ms (irq1=0x%08X)", amp.name, irq1_sts);
                    recordDiagnosticFailure(amp, DIAG_POWER_UP_TIMEOUT, 0x00010010, 0x01000000, irq1_sts);
                }

                if (!sequenceOk || !pupDone) {
                    abortStart();
                    continue;
                }
                if (dspMode) {
                    if (amp.firmwareIdVersion > 0x001C00) {
                        dspCommandOk = sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxSpeakerOutputEnable,
                                                          cirrus::devices::cs35l41::registers::kStatusMailboxRunning);
                        sequenceOk = sequenceOk && dspCommandOk;
                    } else {
                        checkedWrite(0x0000742C, 0x000000F9);
                        checkedWrite(0x00007438, 0x00580941);
                    }

                } else {
                    checkedWrite(0x0000742C, 0x000000F9);
                    checkedWrite(0x00007438, 0x00580941);
                }

                bool locked = lockTestKey(amp);
                if (!sequenceOk || !locked) {
                    abortStart();
                    continue;
                }

                uint32_t readyPwr1 = 0, readyPwr2 = 0, readyDac = 0, readyAsp = 0;
                bool ready = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &readyPwr1) && readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &readyPwr2) &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, &readyDac) &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &readyAsp) && (readyPwr1 & 1) &&
                             (readyPwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == (dspMode ? 0x3001U : 1U) &&
                             readyDac == (dspMode ? 0x32U : 8U) && readyAsp == (dspMode ? 0x10001U : 0x10000U);
                bool streamStillActive = syncAlc287HdaCodec();
                hdaStreamActive = streamStillActive;
                if (!ready) {
                    abortStart();
                    continue;
                }
                if (!streamStillActive || !mHdaState.observed || mHdaState.lastStreamTag != startStreamTag ||
                    mHdaState.lastFormat != startStreamFormat) {
                    stopPlayback(amp);
                    continue;
                }
                if (!checkProtectionStatus(amp)) {
                    abortStart();
                    continue;
                }
                uint32_t pll_sts = 0;
                if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, &pll_sts, TRACE_PLAYBACK) ||
                    !(pll_sts & 2)) {
                    recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, 2, pll_sts);
                    abortStart();
                    continue;
                }

                checkedWrite(cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, dspMode ? 0x00000233 : 0x00000084);
                checkedWrite(cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x00008000);

                uint32_t post_pwr1 = 0, post_pwr2 = 0, post_vol = 0, post_gain = 0;
                bool stateReadable = readRegister(amp, 0x00002014, &post_pwr1) && readRegister(amp, 0x00002018, &post_pwr2) &&
                                     readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &post_vol) &&
                                     readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &post_gain);
                bool startVerified =
                    stateReadable && pupDone && dspCommandOk && sequenceOk && locked && post_vol == 0x00008000 && (pll_sts & 0x02) &&
                    ((post_pwr1 & 0x1) != 0) && ((post_pwr2 & 0x1) != 0) &&
                    ((post_pwr2 & (0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == (dspMode ? 0x00003000U : 0U)) &&
                    (post_gain == (dspMode ? 0x00000233U : 0x00000084U));
                if (startVerified) {
                    amp.playbackActive = true;
                    markDiagnosticSuccess(amp, STAGE_PLAYBACK_ACTIVE);
                    char playbackProperty[80];
                    snprintf(playbackProperty, sizeof(playbackProperty), "Cirrus_Playback_Verdict_%s", amp.name);
                    OSString* playbackVerdict = OSString::withCString("ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED");
                    if (playbackVerdict) {
                        setProperty(playbackProperty, playbackVerdict);
                        playbackVerdict->release();
                    }
                    CIRRUS_LOG("Background Monitor: Playback ENABLED and verified on %s: pwr1=0x%08X pwr2=0x%08X vol=0x%08X gain=0x%08X",
                               amp.name, post_pwr1, post_pwr2, post_vol, post_gain);
                    CIRRUS_LOG("DIAG_BOUNDARY amp=%s HDA_RUN=%d stream_tag=%u format=0x%04X PLL_LOCK=%d PUP_DONE=1 GLOBAL_EN=1 AMP_EN=1; "
                               "speaker converter route and physical audio remain unverified",
                               amp.name, streamStillActive, mHdaState.lastStreamTag, mHdaState.lastFormat, (pll_sts & 2) != 0);
                    snapshotDiagnostics(amp, "PLAYBACK ACTIVE VERIFIED");
                } else {
                    CIRRUS_ERR("Background Monitor: Playback start verification failed on %s (read=%d pup=%d pwr1=0x%08X pwr2=0x%08X "
                               "gain=0x%08X); rolling back",
                               amp.name, stateReadable, pupDone, post_pwr1, post_pwr2, post_gain);
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::cs35l41::registers::kPowerControl1, 0x00000001, post_pwr1);
                    abortStart();
                }
            }
        } else if (!hasAudio && amp.playbackActive) {
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 2) {
                stopPlayback(amp);
            }
        } else {
            amp.playbackStableCount = 0;
        }
    }

    if (mProbeTimer) {
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
        CIRRUS_ERR("Codec 0x%08X on %s is unsupported! Please add -cirrusdbg and report to developer.", deviceId, amp.name);
    }

    if (amp.present) {
        dumpAllRegisters(amp);

        if (bootArgEnabled("-cirrusro"))
            return;

        if (bootArgStrEquals("cirrus_phase", "4A1")) {
            initCodec(amp);
        } else if (bootArgStrEquals("cirrus_phase", "4A2A") || bootArgStrEquals("cirrus_phase", "4A2B") ||
                   bootArgStrEquals("cirrus_phase", "4A2C")) {
            if (initCodec(amp)) {
                initializeHardwareErrata(amp);
            }
        } else if (bootArgStrEquals("cirrus_phase", "4B")) {
            if (initCodec(amp)) {
                if (initializeHardwareErrata(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.finalCrc = calculateRegistersCRC32(amp);
                    dumpAllRegisters(amp);
                }
            }
        } else if (bootArgStrEquals("cirrus_phase", "5A") || bootArgStrEquals("cirrus_phase", "5B") ||
                   bootArgStrEquals("cirrus_phase", "5C.0") || bootArgStrEquals("cirrus_phase", "5C.1") ||
                   bootArgStrEquals("cirrus_phase", "5C.2") || bootArgStrEquals("cirrus_phase", "5C.3") ||
                   bootArgStrEquals("cirrus_phase", "5C.3.5") || bootArgStrEquals("cirrus_phase", "5C.4") ||
                   bootArgStrEquals("cirrus_phase", "5C") || bootArgStrEquals("cirrus_phase", "5D.0")) {
            if (initCodec(amp)) {
                if (initializeHardwareErrata(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.finalCrc = calculateRegistersCRC32(amp);
                    discoverFirmware(amp);
                    if (bootArgStrEquals("cirrus_phase", "5B")) {
                        bringupDSP(amp);
                    } else {
                        char phaseArg[16] = {0};
                        if (PE_parse_boot_argn("-cirrusphase", phaseArg, sizeof(phaseArg))) {
                            if (strncmp(phaseArg, "5D", 2) == 0) {
                            } else if (strncmp(phaseArg, "5C", 2) == 0) {
                                uploadFirmware(amp, phaseArg);

                                bool shouldBootDsp = (strncmp(phaseArg, "5C.4", 4) == 0);
                                if (shouldBootDsp) {
                                    CIRRUS_LOG("bringing up dsp after firmware upload on %s", amp.name);
                                    bringupDSP(amp);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    CIRRUS_LOG("amp %s devid=0x%08X revision=0x%08X present=%s", amp.name, amp.deviceId, amp.revisionId, amp.present ? "yes" : "no");
}

bool CirrusAudioFixup::transferToAddress(UInt8 address, UInt8* writeBuffer, UInt16 writeLength, UInt8* readBuffer, UInt16 readLength) {
    if (address > 0x7F || (writeLength && !writeBuffer) || (readLength && !readBuffer) || (!writeLength && !readLength)) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    if (!mProvider || !mPowerAvailable || mStopping) {
        mLastTransferReturn = kIOReturnNotReady;
        CIRRUS_ERR("transfer blocked: provider=%p powered=%d stopping=%d", mProvider, mPowerAvailable, mStopping);
        return false;
    }

    VoodooI2CAddressedTransfer request{};
    request.address = address;
    request.writeBuffer = writeBuffer;
    request.writeLength = writeLength;
    request.readBuffer = readBuffer;
    request.readLength = readLength;

    setProperty("CirrusTransferCalled", kOSBooleanTrue);
    IOReturn ret = mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS, true, &request, nullptr, nullptr, nullptr);
    mLastTransferReturn = ret;
    setProperty("CirrusTransferRet", (uint64_t)ret, 32);
    if (ret != kIOReturnSuccess) {
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

void CirrusAudioFixup::publishDriverVerdict() {
    bool left = mAmps[0].initialized;
    bool right = mAmps[1].initialized;
    bool bothDsp = left && right && mAmps[0].dspAlive && mAmps[1].dspAlive && mAmps[0].firmwareValidated && mAmps[1].firmwareValidated;
    const char* verdict = !mPowerAvailable                                         ? "SUSPENDED"
                          : bootArgEnabled("-cirrusro")                            ? "READ_ONLY"
                          : (mAmps[0].playbackFaulted || mAmps[1].playbackFaulted) ? "FAULT_LATCHED"
                          : (!left || !right)                                      ? "FAILED_INIT"
                          : bothDsp                                                ? "READY_DSP"
                          : bootArgEnabled("-cirrusnodsp")                         ? "READY_BYPASS_EXPLICIT"
                                                                                   : "OUTPUT_DISABLED_DSP_UNVERIFIED";
    OSString* value = OSString::withCString(verdict);
    if (value) {
        setProperty("Cirrus_Driver_Verdict", value);
        value->release();
    }
    CIRRUS_LOG("DIAG_VERDICT driver=%s left[first=%s latest=%s last_good=%s] right[first=%s latest=%s last_good=%s]", verdict,
               failureName(mAmps[0].diagnostic.firstFailure), failureName(mAmps[0].diagnostic.latestFailure),
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
        IODelay(1000);
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

bool CirrusAudioFixup::sendMailboxCommand(AmplifierState& amp, UInt32 command, UInt32 expectedStatus) {
    if (!writeRegister(amp, 0x00013020, command, TRACE_PLAYBACK)) {
        recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, 0x00013020, command, 0, mLastTransferReturn);
        return false;
    }

    UInt32 status = 0;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        IODelay(1000);
        if (!readRegister(amp, cirrus::devices::cs35l41::registers::kDspMbox2Register, &status, TRACE_PLAYBACK))
            return false;
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
        CIRRUS_ERR("Codec 0x%08X on %s is unsupported! Please add -cirrusdbg and report to developer.", amp.deviceId, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
        return false;
    }

    bool deepDiag = bootArgEnabled("-cirrusdiag");
    UInt32 crc_before = deepDiag ? calculateRegistersCRC32(amp) : 0;

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

    UInt32 devid_after = 0, revid_after = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDeviceId, &devid_after) ||
        !readRegister(amp, cirrus::devices::cs35l41::registers::kRegRevisionId, &revid_after)) {
        CIRRUS_ERR("failed to identify %s after reset", amp.name);
        return false;
    }

    amp.deviceId = devid_after;
    amp.revisionId = revid_after;
    amp.present = (devid_after == cirrus::devices::cs35l41::registers::kValDeviceId);
    if (!amp.present) {
        CIRRUS_ERR("Codec 0x%08X on %s is unsupported after reset! Please add -cirrusdbg and report to developer.", devid_after, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::cs35l41::registers::kValDeviceId, devid_after);
        return false;
    }

    UInt32 crc_after = deepDiag ? calculateRegistersCRC32(amp) : 0;

    char propName[64];
    if (deepDiag) {
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Before_%s", amp.name);
        setProperty(propName, crc_before, 32);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_After_%s", amp.name);
        setProperty(propName, crc_after, 32);
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X crc=0x%08X", amp.name, devid_after, revid_after, crc_after);
    } else {
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X", amp.name, devid_after, revid_after);
    }

    UInt32 rev_only = revid_after & 0xFF;

    switch (rev_only) {
    case 0xB0:
    case 0xB1:
    case 0xB2:
        CIRRUS_LOG("revision 0x%02X verified for %s", rev_only, amp.name);
        break;
    default:
        CIRRUS_LOG("unknown chip revision 0x%02X found on %s", rev_only, amp.name);
        break;
    }

    return true;
}

static uint32_t crc32_le(uint32_t crc, uint8_t const* buf, size_t len) {
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
            crc = crc32_le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32_le(crc, (const uint8_t*)&val, 4);
        }
    }
    return crc;
}

void CirrusAudioFixup::dumpAllRegisters(AmplifierState& amp) {
    bool compact = bootArgEnabled("cirrus_dump_compact");
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
            crc = crc32_le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32_le(crc, (const uint8_t*)&val, 4);

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

    IOSleep(1000);
    uint32_t crcT1 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t1 checksum for %s: 0x%08X", amp.name, crcT1);

    IOSleep(4000);
    uint32_t crcT5 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t5 checksum for %s: 0x%08X", amp.name, crcT5);

    IOSleep(25000);
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
    bool deepDiag = bootArgEnabled("cirrus_deepdiag");
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc);
    size_t allocSize = numRegs * sizeof(UInt32);

    char propName[64] = {0};
    UInt32 crc_unlock = 0;
    UInt32 crc_errata = 0;
    UInt32 crc_otp = 0;
    UInt32 crc_lock = 0;

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
        crc_unlock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
        setProperty(propName, crc_unlock, 32);
        CIRRUS_LOG("checksum after unlock on %s: 0x%08X", amp.name, crc_unlock);
    }

    if (!applyErrataPatch(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot2);
        CIRRUS_LOG("register diffs after applying errata patch on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot1, snapshot2);
        crc_errata = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
        setProperty(propName, crc_errata, 32);
        CIRRUS_LOG("checksum after errata patch on %s: 0x%08X", amp.name, crc_errata);
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
        crc_otp = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
        setProperty(propName, crc_otp, 32);
        CIRRUS_LOG("checksum after otp unpack on %s: 0x%08X", amp.name, crc_otp);
    }

    if (!lockTestKey(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        crc_lock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
        setProperty(propName, crc_lock, 32);
        CIRRUS_LOG("checksum after locking test keys on %s: 0x%08X", amp.name, crc_lock);
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
    const ErrataTable errata_tables[] = {{0xB2, cs35l41_revb2_errata_patch, sizeof(cs35l41_revb2_errata_patch) / sizeof(ErrataPatch)}};

    UInt32 rev_only = amp.revisionId & 0xFF;
    const ErrataTable* table_to_apply = nullptr;

    for (size_t i = 0; i < sizeof(errata_tables) / sizeof(ErrataTable); i++) {
        if (errata_tables[i].revid == rev_only) {
            table_to_apply = &errata_tables[i];
            break;
        }
    }

    if (!table_to_apply) {
        CIRRUS_ERR("no errata patches found for revision 0x%02X on %s", rev_only, amp.name);
        return false;
    }

    CIRRUS_LOG("applying %lu errata patches for revision 0x%02X on %s", table_to_apply->numPatches, rev_only, amp.name);

    for (size_t i = 0; i < table_to_apply->numPatches; i++) {
        if (!writeRegister(amp, table_to_apply->patches[i].reg, table_to_apply->patches[i].value)) {
            CIRRUS_ERR("failed to apply errata patch at register 0x%08X on %s", table_to_apply->patches[i].reg, amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA, table_to_apply->patches[i].reg, table_to_apply->patches[i].value, 0,
                                    mLastTransferReturn);
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
    for (int i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
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

    for (int i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
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
        if (sequence[i].delay_us > 0) {
            IODelay(sequence[i].delay_us);
        }
    }
    return true;
}

bool CirrusAudioFixup::applyPLL(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    if (!device.setupPLL(io)) {
        CIRRUS_LOG("warning: PLL did NOT lock on %s. I2S clocks might be inactive.", amp.name);
    } else {
        CIRRUS_LOG("PLL lock verified successfully on %s", amp.name);
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_PLL_CRC_right", crc_after, 32);
        setProperty("Cirrus_PLL_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_PLL_CRC_left", crc_after, 32);
        setProperty("Cirrus_PLL_TimeUS_left", elapsed_us, 32);
    }

    return true;
}

bool CirrusAudioFixup::applyASP(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    device.setupASP(io, strcmp(amp.name, "right") == 0, amp.firmwareValidated);

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_ASP_CRC_right", crc_after, 32);
        setProperty("Cirrus_ASP_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_ASP_CRC_left", crc_after, 32);
        setProperty("Cirrus_ASP_TimeUS_left", elapsed_us, 32);
    }

    return true;
}

bool CirrusAudioFixup::applyGPIO(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);

    device.setupGPIO(io);

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_GPIO_CRC_right", crc_after, 32);
        setProperty("Cirrus_GPIO_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_GPIO_CRC_left", crc_after, 32);
        setProperty("Cirrus_GPIO_TimeUS_left", elapsed_us, 32);
    }

    return true;
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
                    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, parent);
                    if (pci) {
                        const char* name = pci->getName();
                        uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
                        uint16_t device = pci->configRead16(kIOPCIConfigDeviceID);
                        int score = 0;
                        if (vendor == 0x1022 && device == 0x15E3)
                            score += 200;
                        if (name && strcmp(name, "HDEF") == 0)
                            score += 100;
                        else if (name && strcmp(name, "HDAS") == 0)
                            score += 80;
                        else if (name && strcmp(name, "HDAU") == 0)
                            score -= 100;
                        if (vendor == 0x1022)
                            score += 40;
                        else if (vendor == 0x10DE)
                            score -= 40;
                        if (vendor == 0x1022 && device == 0x15E3 && score > bestHdaScore) {
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
        uint32_t pciDevice = 0;
        IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, service);
        if (pci) {
            pciVendor = pci->configRead16(kIOPCIConfigVendorID);
            pciDevice = pci->configRead16(kIOPCIConfigDeviceID);
        } else {
            OSData* venData = OSDynamicCast(OSData, service->getProperty("vendor-id"));
            if (venData && venData->getLength() >= 4) {
                pciVendor = *((uint32_t*)venData->getBytesNoCopy()) & 0xFFFF;
            } else if (venData && venData->getLength() >= 2) {
                pciVendor = *((uint16_t*)venData->getBytesNoCopy());
            }
            OSData* devData = OSDynamicCast(OSData, service->getProperty("device-id"));
            if (devData && devData->getLength() >= 4) {
                pciDevice = *((uint32_t*)devData->getBytesNoCopy()) & 0xFFFF;
            } else if (devData && devData->getLength() >= 2) {
                pciDevice = *((uint16_t*)devData->getBytesNoCopy());
            }
        }
        if (pciVendor == 0xFFFF || pciVendor == 0xFFFFFFFF || pciVendor == 0) {
            continue;
        }

        int score = 0;
        if ((pciVendor & 0xFFFF) == 0x1022 && (pciDevice & 0xFFFF) == 0x15E3)
            score += 200;

        OSData* classCodeData = OSDynamicCast(OSData, service->getProperty("class-code"));
        if (classCodeData && classCodeData->getLength() >= 3) {
            const uint8_t* bytes = (const uint8_t*)classCodeData->getBytesNoCopy();

            if (bytes[2] == 0x04 && bytes[1] == 0x03) {
                score += 10;
            }
        }

        const char* name = service->getName();
        if (name) {
            if (strcmp(name, "HDEF") == 0)
                score += 5;
            else if (strcmp(name, "HDAS") == 0)
                score += 3;
            else if (strcmp(name, "HDAU") == 0)
                score -= 10;
        }

        if (pciVendor == 0x1022 && pciDevice == 0x15E3 && score >= 10 && score > bestScore) {
            bestScore = score;
            bestController = service;
        }
    }

    if (bestController) {
        bestController->retain();
    }
    iter->release();
    return bestScore >= 10 ? bestController : nullptr;
}

IOService* CirrusAudioFixup::getAudioController() {
    return audioController();
}

static uint32_t calculate_crc32(const uint8_t* data, size_t length) {
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

void CirrusAudioFixup::discoverFirmware(AmplifierState& amp) {
    CIRRUS_LOG("discovering firmware for amplifier: %s", amp.name);

    amp.firmwareValidated = false;
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
                    if ((rawSub >> 16) == 0x17AA) {
                        subVendor = rawSub >> 16;
                        subDevice = rawSub & 0xFFFF;
                    } else if ((rawSub & 0xFFFF) == 0x17AA) {
                        subVendor = rawSub & 0xFFFF;
                        subDevice = rawSub >> 16;
                    } else {
                        subDevice = rawSub & 0xFFFF;
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
        if (subVendor == 0x17AA && subDevice == 0x382B) {
            subDevice = 0x3847;
            ssid = 0x17AA3847;
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
    CIRRUS_LOG("Firmware identity %s: SSID=0x%08X speaker=%u source=%s", amp.name, ssid, spkid,
               explicitSpeaker ? "BOOT_ARGUMENT" : "BOARD_PROFILE_ASSUMPTION");

    char propSSID[64];
    snprintf(propSSID, sizeof(propSSID), "Cirrus_SSID_%s", amp.name);
    setProperty(propSSID, (uint64_t)ssid, 32);

    const FirmwareResource* foundRes = nullptr;
    for (size_t i = 0; i < firmwareTableSize; i++) {
        if (firmwareTable[i].subsystemVendor == subVendor && firmwareTable[i].subsystemDevice == subDevice &&
            (firmwareTable[i].spkid == spkid || firmwareTable[i].spkid == 0)) {
            foundRes = &firmwareTable[i];
            break;
        }
    }

    if (!foundRes) {
        CIRRUS_ERR("Unsupported audio hardware: Subsystem ID 0x%08X (vendor=0x%04X, device=0x%04X, spkid=%u) on %s", ssid, subVendor,
                   subDevice, spkid, amp.name);
        if (!gCirrusDebug) {
            CIRRUS_ERR("To dump full hardware profile, add '-cirrusdbg' to boot-args and reboot");
            CIRRUS_ERR("Report your hardware profile to: https://github.com/hoaugtr/CirrusAudioFixup/issues");
        } else {
            IOLog(CIRRUS_LOG_PREFIX "=== CIRRUS AUDIO HARDWARE PROFILE DUMP ===\n");
            IOLog(CIRRUS_LOG_PREFIX "SSID: 0x%08X (Vendor: 0x%04X, Device: 0x%04X)\n", ssid, subVendor, subDevice);
            IOLog(CIRRUS_LOG_PREFIX "Amp: %s, I2C Address: 0x%02X\n", amp.name, amp.address);
            IOLog(CIRRUS_LOG_PREFIX "Hardware ID: DEVID=0x%08X REVID=0x%08X\n", amp.deviceId, amp.revisionId);
            IOLog(CIRRUS_LOG_PREFIX "Speaker ID: %u (explicit=%d)\n", spkid, explicitSpeaker ? 1 : 0);
            IOLog(CIRRUS_LOG_PREFIX "HDA Stream: Tag=%u Format=0x%04X\n", mHdaState.lastStreamTag, mHdaState.lastFormat);
            IOLog(CIRRUS_LOG_PREFIX "=== END HARDWARE PROFILE DUMP ===\n");
        }
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 0x17AA3847, ssid);
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
    OSString* srcStr = OSString::withCString("Database");
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

    bool isRight = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight);
    const uint8_t* chanBin = (isRight && foundRes->binRight) ? foundRes->binRight : foundRes->bin;
    size_t chanBinSize = (isRight && foundRes->binRight) ? foundRes->binRightSize : foundRes->binSize;

    uint32_t fwCrc = calculate_crc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculate_crc32(chanBin, chanBinSize);
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
               calculate_crc32(foundRes->bin, foundRes->binSize),
               foundRes->binRight ? calculate_crc32(foundRes->binRight, foundRes->binRightSize) : 0, foundRes->binRight ? 1 : 0);
    OSString* statusStr = OSString::withCString("READY");
    if (statusStr) {
        setProperty(propStatus, statusStr);
        statusStr->release();
    }
}

bool CirrusAudioFixup::bringupDSP(AmplifierState& amp) {
    if (!amp.firmwareValidated || !amp.wmfwData || !amp.wmfwSize) {
        CIRRUS_ERR("missing firmware data on %s", amp.name);
        return false;
    }

    cirrus::firmware::FirmwareImage wmfwImage;
    if (!cirrus::firmware::CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, &wmfwImage)) {
        CIRRUS_ERR("failed to parse WMFW on %s", amp.name);
        return false;
    }

    cirrus::firmware::FirmwareImage binImage;
    if (amp.binData && amp.binSize) {
        if (!cirrus::firmware::CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, &binImage)) {
            CIRRUS_ERR("failed to parse BIN on %s", amp.name);
        }
    }

    cirrus::firmware::MappedImage mappedWmfw;
    if (!cirrus::firmware::CirrusFirmwareMapper::mapFirmwareImage(wmfwImage, mappedWmfw)) {
        CIRRUS_ERR("failed to map WMFW image on %s", amp.name);
        return false;
    }

    cirrus::firmware::MappedImage mappedBin;
    if (amp.binData && amp.binSize && binImage.regionCount > 0) {
        cirrus::firmware::CirrusFirmwareMapper::mapFirmwareImage(binImage, mappedBin);
    }

    FixupRegisterIOAdapter io(this, amp);
    cirrus::firmware::UploadSession sessionWmfw;
    if (!cirrus::firmware::CirrusFirmwareScheduler::run(amp.name, io, mappedWmfw, sessionWmfw)) {
        CIRRUS_ERR("failed to upload WMFW on %s", amp.name);
        return false;
    }

    if (amp.binData && amp.binSize && mappedBin.regionCount > 0) {
        cirrus::firmware::UploadSession sessionBin;
        if (!cirrus::firmware::CirrusFirmwareScheduler::run(amp.name, io, mappedBin, sessionBin)) {
            CIRRUS_ERR("failed to upload BIN on %s", amp.name);
            return false;
        }
    }

    // Command DSP to boot!
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
    if (!device.bootDsp(io, amp.wmfwData, amp.wmfwSize, amp.binData, amp.binSize)) {
        CIRRUS_ERR("DSP boot failed on %s", amp.name);
        return false;
    }

    amp.playbackFaulted = false;
    CIRRUS_LOG("firmware upload complete on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::verifyDSPAlive(AmplifierState& amp) {
    uint32_t core_ctrl = 0, sys_id = 0, mbox = 0, halo_state = 0;
    bool core_pass = false, reset_pass = false, sysid_pass = false, mbox_pass = false, xm_pass = false;

    bool readable = amp.haloStateRegister && amp.haloHeartbeatRegister &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core_ctrl, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sys_id, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &mbox, TRACE_DUMP) &&
                    readRegister(amp, amp.haloStateRegister, &halo_state, TRACE_DUMP);

    if (core_ctrl & cirrus::devices::cs35l41::registers::kValHaloCoreEnable)
        core_pass = true;
    if ((core_ctrl & cirrus::devices::cs35l41::registers::kValHaloCoreReset) == 0)
        reset_pass = true;
    if (sys_id != 0x00000000 && sys_id != 0xFFFFFFFF)
        sysid_pass = true;

    if (mbox == cirrus::devices::cs35l41::registers::kStatusMailboxPaused)
        mbox_pass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xm_pass = true;
    }

    uint32_t xm_vio = 0, xm_vio_addr = 0;
    uint32_t ym_vio = 0, ym_vio_addr = 0;
    uint32_t pm_vio = 0, pm_vio_addr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    readable = readRegister(amp, 0x2BC3104, &xm_vio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xm_vio_addr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ym_vio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ym_vio_addr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pm_vio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pm_vio_addr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP) && readable;

    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpu_clean = ((xm_vio & MPU_VIO_MASK) == 0 && (ym_vio & MPU_VIO_MASK) == 0 && (pm_vio & MPU_VIO_MASK) == 0);

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, sysid=%s(0x%08X), "
               "mailbox=%s(fw_status=%u), halo_state=0x%08X, xm_read=%s",
               amp.name, reset_pass ? "ok" : "fail", core_pass ? "ok" : "fail", sysid_pass ? "ok" : "fail", sys_id,
               mbox_pass ? "ok" : "fail", mbox, halo_state, xm_pass ? "ok" : "fail");
    CIRRUS_LOG("dsp verify MPU/scratch for %s: mpu=%s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X "
               "pm_vio=0x%08X@0x%08X scratch=[0x%08X 0x%08X 0x%08X 0x%08X]",
               amp.name, mpu_clean ? "clean" : "FAULT", xm_vio, xm_vio_addr, ym_vio, ym_vio_addr, pm_vio, pm_vio_addr, sc1, sc2, sc3, sc4);

    uint32_t hb0 = 0, hb1 = 0;
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb0, TRACE_DUMP) && readable;
    IODelay(2000);
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb1, TRACE_DUMP) && readable;

    const uint32_t HALO_STATE_RUN = 2;
    bool run_pass = (halo_state == HALO_STATE_RUN);
    if (!run_pass || !mpu_clean || !mbox_pass) {
        CIRRUS_ERR("dsp verdict for %s: halo=0x%08X heartbeat=0x%08X->0x%08X mpu=%s; DSP mode disabled", amp.name, halo_state, hb0, hb1,
                   mpu_clean ? "clean" : "FAULT");
    }

    return readable && core_pass && reset_pass && sysid_pass && mbox_pass && xm_pass && run_pass && mpu_clean;
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
        if (!bulkRead(amp, 0x02800000 + offset, table + offset, chunk, TRACE_FIRMWARE))
            return false;
        offset += chunk;
    }
    return CirrusFirmwareParser::parseAlgorithmTable(table, length, outImage);
}

void CirrusAudioFixup::uploadFirmware(AmplifierState& amp, const char* phaseArg) {
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
    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
    return device.configureHardware(io);
}

void CirrusAudioFixup::logASPSnapshot(AmplifierState& amp) {
    {
        uint32_t enables = 0, rate = 0, fmt = 0, hiz = 0;
        uint32_t tx_wl = 0, rx_wl = 0, tx_slot = 0, rx_slot = 0;
        uint32_t rx1_src = 0, rx2_src = 0, rx3_src = 0, rx4_src = 0, rx5_src = 0;

        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &enables);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, &rate);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, &fmt);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, &hiz);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFrameTxSlot, &tx_slot);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFrameRxSlot, &rx_slot);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortTxWordLength, &tx_wl);
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortRxWordLength, &rx_wl);
        readRegister(amp, 0x00004C40, &rx1_src);
        readRegister(amp, 0x00004C44, &rx2_src);
        readRegister(amp, 0x00004C48, &rx3_src);
        readRegister(amp, 0x00004C4C, &rx4_src);
        readRegister(amp, 0x00004C50, &rx5_src);

        CIRRUS_LOG("asp snapshot for %s: enables=0x%08X rate=0x%08X format=0x%08X hiz=0x%08X", amp.name, enables, rate, fmt, hiz);
        CIRRUS_LOG("asp snapshot slots for %s: rx_slot=0x%08X tx_slot=0x%08X rx_wl=0x%08X tx_wl=0x%08X", amp.name, rx_slot, tx_slot, rx_wl,
                   tx_wl);
        CIRRUS_LOG("asp snapshot routing for %s: rx1_src=0x%08X rx2_src=0x%08X rx3_src=0x%08X rx4_src=0x%08X rx5_src=0x%08X", amp.name,
                   rx1_src, rx2_src, rx3_src, rx4_src, rx5_src);

        bool pass = true;
        uint32_t expected_rx_slot = (strcmp(amp.name, "right") == 0) ? 1 : 0;
        if ((rx_slot & 0x3F) != expected_rx_slot) {
            CIRRUS_ERR("asp rx slot mismatch on %s", amp.name);
            pass = false;
        }

        uint32_t expected_rx1_src = 0x08;
        uint32_t expected_rx2_src = (amp.monitorCount >= 1) ? 0x08 : 0x09;
        if (rx1_src != expected_rx1_src) {
            CIRRUS_ERR("asp rx1 source routing mismatch on %s", amp.name);
            pass = false;
        }
        if (rx2_src != expected_rx2_src) {
            CIRRUS_ERR("asp rx2 source routing mismatch on %s", amp.name);
            pass = false;
        }

        if (pass) {
            CIRRUS_LOG("asp validation check: pass on %s", amp.name);
        } else {
            CIRRUS_ERR("asp validation check: fail on %s", amp.name);
        }
    }

    void CirrusAudioFixup::logDSPSnapshot(AmplifierState & amp) {
        uint32_t dsp_state = 0, mbox1 = 0, mbox2 = 0;
        readRegister(amp, 0x00013004, &mbox2);
        readRegister(amp, 0x00013020, &mbox1);
        readRegister(amp, 0x02BC1000, &dsp_state);

        CIRRUS_LOG("dsp status snapshot for %s: state=0x%08X core=0x%08X mbox1=%u mbox2=%u", amp.name, mbox2, dsp_state, mbox1, mbox2);
    }

    void CirrusAudioFixup::logDSPBootReport(AmplifierState & amp) {
        uint32_t core_ctrl = 0, sys_id = 0, halo_state = 0;
        uint32_t mbox1 = 0, mbox2 = 0;
        uint32_t xm_vio = 0, ym_vio = 0, pm_vio = 0;
        uint32_t xm_vio_addr = 0, ym_vio_addr = 0, pm_vio_addr = 0;
        uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
        uint32_t ts0 = 0, ts1 = 0, ts2 = 0;

        bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core_ctrl, TRACE_DUMP);
        readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sys_id, TRACE_DUMP) && readable;
        readable = amp.haloStateRegister && readRegister(amp, amp.haloStateRegister, &halo_state, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC3104, &xm_vio, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC3100, &xm_vio_addr, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC310C, &ym_vio, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC3108, &ym_vio_addr, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC3114, &pm_vio, TRACE_DUMP) && readable;
        readable = readRegister(amp, 0x2BC3110, &pm_vio_addr, TRACE_DUMP) && readable;
        readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP);
        readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP);
        readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP);
        readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP);

        bool hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts0, TRACE_DUMP);
        IODelay(2000);
        hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts1, TRACE_DUMP) && hbReadable;
        IODelay(2000);
        hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts2, TRACE_DUMP) && hbReadable;

        bool core_en = (core_ctrl & cirrus::devices::cs35l41::registers::kValHaloCoreEnable) != 0;
        bool core_out_of_reset = (core_ctrl & cirrus::devices::cs35l41::registers::kValHaloCoreReset) == 0;
        bool sysid_ok = (sys_id != 0x00000000 && sys_id != 0xFFFFFFFF);
        bool run_ok = (halo_state == 2);
        bool mbox_ok = (mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxRunning ||
                        mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxPaused);
        const uint32_t MPU_VIO_MASK = 0x007E0000;
        bool mpu_clean = ((xm_vio & MPU_VIO_MASK) == 0 && (ym_vio & MPU_VIO_MASK) == 0 && (pm_vio & MPU_VIO_MASK) == 0);
        bool hb_alive = (ts0 != ts1 && ts1 != ts2);

        const char* verdict;
        const char* rootCause;
        if (!readable) {
            verdict = "UNVERIFIED";
            rootCause = "required register read failed or control unresolved";
        } else if (!amp.firmwareValidated || !amp.dspAlive) {
            verdict = "DISABLED";
            rootCause = "initialization failed; current registers may reflect cleanup";
        } else if (core_en && core_out_of_reset && sysid_ok && run_ok && mpu_clean && mbox_ok) {
            verdict = "HEALTHY";
            rootCause = "HALO RUN and mailbox verified; heartbeat is diagnostic only";
        } else {
            verdict = "FAIL";
            rootCause = "required DSP state invariant failed; inspect raw registers";
        }

        CIRRUS_LOG("===== DSP BOOT REPORT for %s =====", amp.name);
        CIRRUS_LOG("  VERDICT   : %s", verdict);
        CIRRUS_LOG("  ROOT CAUSE: %s", rootCause);
        CIRRUS_LOG("  core      : ctrl=0x%08X en=%s reset_released=%s", core_ctrl, core_en ? "yes" : "NO",
                   core_out_of_reset ? "yes" : "NO");
        CIRRUS_LOG("  sys_id    : 0x%08X (%s)", sys_id, sysid_ok ? "ok" : "BAD");
        CIRRUS_LOG("  halo_state: 0x%08X (%s, want RUN=2)", halo_state, run_ok ? "RUN" : "not-run");
        CIRRUS_LOG("  mailbox   : mbox1=0x%08X mbox2=0x%08X (%s)", mbox1, mbox2, mbox_ok ? "ok" : "not-running");
        CIRRUS_LOG("  mpu       : %s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X pm_vio=0x%08X@0x%08X", mpu_clean ? "clean" : "FAULT", xm_vio,
                   xm_vio_addr, ym_vio, ym_vio_addr, pm_vio, pm_vio_addr);
        CIRRUS_LOG("  scratch   : [0x%08X 0x%08X 0x%08X 0x%08X] (firmware panic code if non-zero)", sc1, sc2, sc3, sc4);
        CIRRUS_LOG("  heartbeat : t0=0x%08X t1=0x%08X t2=0x%08X (%s)", ts0, ts1, ts2,
                   !hbReadable ? "READ_FAILED"
                   : hb_alive  ? "CHANGING"
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

    void CirrusAudioFixup::snapshotPlayback(AmplifierState & amp) {
        uint32_t irq1_sts1 = 0, irq1_sts2 = 0, irq1_sts3 = 0, irq1_sts4 = 0;
        uint32_t pwrmgt_sts = 0;

        readRegister(amp, 0x00010010, &irq1_sts1);
        readRegister(amp, 0x00010014, &irq1_sts2);
        readRegister(amp, 0x00010018, &irq1_sts3);
        readRegister(amp, 0x0001001C, &irq1_sts4);
        readRegister(amp, 0x00002908, &pwrmgt_sts);

        CIRRUS_LOG("playback interrupts snapshot for %s: irq1=0x%08X irq2=0x%08X irq3=0x%08X irq4=0x%08X power_status=0x%08X", amp.name,
                   irq1_sts1, irq1_sts2, irq1_sts3, irq1_sts4, pwrmgt_sts);
    }

    void CirrusAudioFixup::snapshotDiagnostics(AmplifierState & amp, const char* stage) {
        uint32_t irq1[4] = {0}, irq1_mask[4] = {0};
        uint32_t irq2[4] = {0}, irq2_mask[4] = {0};
        uint32_t sp_en = 0, sp_rate = 0, sp_fmt = 0, sp_hiz = 0;
        uint32_t pwr_sts = 0, strm_err = 0;

        CIRRUS_LOG("diagnostics dump (%s) for amplifier %s:", stage, amp.name);

        readRegister(amp, 0x00010010, &irq1[0]);
        readRegister(amp, 0x00010014, &irq1[1]);
        readRegister(amp, 0x00010018, &irq1[2]);
        readRegister(amp, 0x0001001C, &irq1[3]);

        uint32_t raw_sts3 = 0;
        readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, &raw_sts3);
        bool pup_done = (irq1[0] & 0x01000000) != 0;
        bool amp_short = (irq1[0] & 0x80000000) != 0;
        bool dsp_error = (irq1[0] & 0x00000002) != 0;
        bool pll_lock = (raw_sts3 & 0x00000002) != 0;

        CIRRUS_LOG("decoded interrupts on %s: pup_done=%d amp_short=%d dsp_err=%d pll_lock=%d", amp.name, pup_done, amp_short, dsp_error,
                   pll_lock);
        CIRRUS_LOG("irq1 status values on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X raw3=0x%08X", amp.name, irq1[0], irq1[1], irq1[2],
                   irq1[3], raw_sts3);

        readRegister(amp, 0x00010110, &irq1_mask[0]);
        readRegister(amp, 0x00010114, &irq1_mask[1]);
        readRegister(amp, 0x00010118, &irq1_mask[2]);
        readRegister(amp, 0x0001011C, &irq1_mask[3]);
        CIRRUS_LOG("irq1 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq1_mask[0], irq1_mask[1], irq1_mask[2],
                   irq1_mask[3]);

        readRegister(amp, 0x00010810, &irq2[0]);
        readRegister(amp, 0x00010814, &irq2[1]);
        readRegister(amp, 0x00010818, &irq2[2]);
        readRegister(amp, 0x0001081C, &irq2[3]);
        CIRRUS_LOG("irq2 status registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2[0], irq2[1], irq2[2], irq2[3]);

        readRegister(amp, 0x00010910, &irq2_mask[0]);
        readRegister(amp, 0x00010914, &irq2_mask[1]);
        readRegister(amp, 0x00010918, &irq2_mask[2]);
        readRegister(amp, 0x0001091C, &irq2_mask[3]);
        CIRRUS_LOG("irq2 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2_mask[0], irq2_mask[1], irq2_mask[2],
                   irq2_mask[3]);

        readRegister(amp, 0x00004800, &sp_en);
        readRegister(amp, 0x00004804, &sp_rate);
        readRegister(amp, 0x00004808, &sp_fmt);
        readRegister(amp, 0x0000480C, &sp_hiz);
        CIRRUS_LOG("serial port configuration for %s: enables=0x%08X rate=0x%08X", amp.name, sp_en, sp_rate);
        CIRRUS_LOG("serial port format for %s: format=0x%08X hiz=0x%08X", amp.name, sp_fmt, sp_hiz);

        readRegister(amp, 0x00002908, &pwr_sts);
        readRegister(amp, 0x02BC5A08, &strm_err);
        CIRRUS_LOG("power status on %s: power_mgt=0x%08X arb_error=0x%08X", amp.name, pwr_sts, strm_err);

        uint32_t dsp_ts = 0, mdsync_rx = 0;
        readRegister(amp, 0x025C0800, &dsp_ts);
        readRegister(amp, 0x00003420, &mdsync_rx);
        CIRRUS_LOG("dsp execution registers on %s: timestamp=0x%08X mdsync=0x%08X", amp.name, dsp_ts, mdsync_rx);

        uint32_t dsp_mbox1 = 0, dsp_mbox2 = 0;
        readRegister(amp, 0x00013020, &dsp_mbox1);
        readRegister(amp, 0x00013004, &dsp_mbox2);
        CIRRUS_LOG("mailbox states on %s: mbox1=0x%08X mbox2=0x%08X", amp.name, dsp_mbox1, dsp_mbox2);

        for (uint32_t i = 0; i < amp.diagnosticControlCount; i++) {
            uint32_t val = 0;
            bool valid = readRegister(amp, amp.diagnosticControls[i].address, &val);
            CIRRUS_LOG("control register on %s: %s (0x%08X) valid=%d value=0x%08X", amp.name, amp.diagnosticControls[i].name,
                       amp.diagnosticControls[i].address, valid, val);
        }
    }

    void CirrusAudioFixup::logPowerSnapshot(AmplifierState & amp) {
        uint32_t pwr_ctrl1 = 0, pwr_ctrl2 = 0, pwr_ctrl3 = 0;
        readRegister(amp, 0x00002014, &pwr_ctrl1);
        readRegister(amp, 0x00002018, &pwr_ctrl2);
        readRegister(amp, 0x0000201C, &pwr_ctrl3);

        CIRRUS_LOG("power rails snapshot for %s: ctrl1=0x%08X ctrl2=0x%08X ctrl3=0x%08X", amp.name, pwr_ctrl1, pwr_ctrl2, pwr_ctrl3);

        bool pass = true;

        if ((pwr_ctrl1 & 1) != 0) {
            CIRRUS_ERR("global enable unexpectedly active while idle on %s", amp.name);
            pass = false;
        }
        if ((pwr_ctrl2 & 1) != 0) {
            CIRRUS_ERR("amplifier stage unexpectedly enabled while idle on %s", amp.name);
            pass = false;
        }

        if (pass) {
            CIRRUS_LOG("idle power verify results: pass on %s (GLOBAL_EN safely off)", amp.name);
        } else {
            CIRRUS_ERR("power verify results: fail on %s", amp.name);
        }
    }

    bool CirrusAudioFixup::powerUpAmplifier(AmplifierState & amp) {
        FixupRegisterIOAdapter io(this, amp);
        cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
        return device.powerUpAmplifier(io, amp.firmwareValidated);
    }

    bool CirrusAudioFixup::verifyIdleConfiguration(AmplifierState & amp) {
        FixupRegisterIOAdapter io(this, amp);
        cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
        return device.verifyIdleConfiguration(io);
    }
