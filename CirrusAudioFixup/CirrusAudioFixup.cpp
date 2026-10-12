//
// CirrusAudioFixup.cpp
// IOKit service for board-specific CS35L41 bring-up and playback control.
//
// Hardware readiness is established by register and DSP checks. HDA stream
// observation cannot confirm the downstream audio route or audible output.
// See LICENSE for distribution terms.
//

#include "CirrusAudioFixup.hpp"
#include "Diagnostics/FailureNames.hpp"
#include "Support/CPUIdentity.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

#define super IOService
using namespace cirrus::diagnostics;
OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)

    class CirrusAudioFixup;

class ServiceRegisterIO : public cirrus::core::RegisterIO {
private:
    CirrusAudioFixup* mService;
    AmplifierState& mAmplifier;
    cirrus::diagnostics::TraceSource mSource;

public:
    ServiceRegisterIO(CirrusAudioFixup* fixup, AmplifierState& amp,
                      cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_PROBE)
        : mService(fixup), mAmplifier(amp), mSource(source) {}

    int32_t lastError() const override { return mService->mLastTransferReturn; }

    bool read(uint32_t address, uint32_t* value) override { return mService->readRegister(mAmplifier, address, value, mSource); }

    bool write(uint32_t address, uint32_t value) override { return mService->writeRegister(mAmplifier, address, value, mSource); }

    bool readWithTrace(uint32_t r, uint32_t* v, TraceSource source) override { return mService->readRegister(mAmplifier, r, v, source); }
    bool writeWithTrace(uint32_t r, uint32_t v, TraceSource source) override { return mService->writeRegister(mAmplifier, r, v, source); }
    bool bulkReadWithTrace(uint32_t r, uint8_t* p, size_t n, TraceSource source) override {
        return mService->bulkRead(mAmplifier, r, p, n, source);
    }
    bool bulkWriteWithTrace(uint32_t r, const uint8_t* p, size_t n, TraceSource source) override {
        return mService->bulkWrite(mAmplifier, r, p, n, source);
    }
    bool readForDiagnostic(uint32_t address, uint32_t* value) override {
        return mService->readRegister(mAmplifier, address, value, cirrus::diagnostics::TRACE_DUMP);
    }

    bool bulkReadForDiagnostic(uint32_t address, uint8_t* data, size_t length) override {
        return mService->bulkRead(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_DUMP);
    }

    bool updateBits(uint32_t address, uint32_t mask, uint32_t value) override {
        return mService->updateRegisterBits(mAmplifier, address, mask, value, mSource);
    }

    bool pollBit(uint32_t address, uint32_t mask, uint32_t expected, uint32_t timeoutMs) override {
        return mService->pollRegisterBit(mAmplifier, address, mask, expected, timeoutMs, cirrus::diagnostics::TRACE_PROBE);
    }

    bool bulkRead(uint32_t address, uint8_t* data, size_t length) override {
        return mService->bulkRead(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }

    bool bulkWrite(uint32_t address, const uint8_t* data, size_t length) override {
        return mService->bulkWrite(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }
};

static uint64_t playbackTimeMilliseconds() {
    uint64_t nanos = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
    return nanos / 1000000;
}

bool CirrusAudioFixup::registerAmplifierDrivers() { return cirrus::devices::registerDrivers(mDriverRegistry); }

// Controller construction is selected by verified platform data, independently
// of the AMP definition. A selected factory failure must never choose another
// provider ABI. The returned controller is owned until all ports are released.
cirrus::transport::I2CController* CirrusAudioFixup::createI2CController() {
    if (!mProvider)
        return nullptr;
    if (mPlatformProfile && mPlatformProfile->i2cControllerFactory) {
        auto* controller = mPlatformProfile->i2cControllerFactory(mProvider);
        mAttachment.communication = controller ? "CREATED" : "FAILED";
        if (!controller)
            recordAttachmentFailure("COMMUNICATION", "I2C_FACTORY_REJECTED", kIOReturnNotReady);
        return controller;
    }
    if (!mProvider->metaCast("VoodooI2CDeviceNub")) {
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "I2C_PROVIDER_ABI_UNAVAILABLE", kIOReturnUnsupported);
        return nullptr;
    }
    auto* controller = new cirrus::transport::VoodooI2CController(mProvider);
    mAttachment.communication = controller ? "CREATED" : "FAILED";
    if (!controller)
        recordAttachmentFailure("COMMUNICATION", "I2C_CONTROLLER_ALLOCATION_FAILED", kIOReturnNoMemory);
    return controller;
}

bool CirrusAudioFixup::bindAmplifierDrivers() {
    if (mAmplifierManager.bound())
        return true;
    if (bootArgEnabled("-cirrusro"))
        return false;
    if (!mPlatformAuthorized || !mPlatformProfile || !mAmplifierCount || mAmplifierCount > kMaxAmplifiers)
        return false;
    cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid};
    if (!mNativeSPI.bound()) {
        int length = sizeof(mRuntimeControllerPath);
        if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane))
            return false;
        for (size_t i = 0; i < mAmplifierCount; ++i)
            mAmplifiers[i].endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, mAmplifiers[i].address};
        if (!mI2CController)
            mI2CController = createI2CController();
        if (!mI2CController)
            return false;
    }
    if (mPlatformProfile->boardConfiguration)
        mRuntimeBoard = *mPlatformProfile->boardConfiguration;
    else {
        int length = sizeof(mRuntimeControllerPath);
        if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane)) {
            releaseAmplifierDrivers();
            return false;
        }
        for (size_t i = 0; i < mAmplifierCount; ++i) {
            auto& amp = mAmplifiers[i];
            amp.endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, amp.address};
            mRuntimeBindings[i] = {amp.endpoint, amp.deviceId, uint32_t(amp.channel), 1, amp.name, mPlatformProfile->acpiHid, 2};
        }
        // The legacy allowlist authorizes only its compiled reference routing,
        // reset/power and firmware-selection plan. No new board is inferred.
        mRuntimeBoard = {actual, mRuntimeBindings, mAmplifierCount, true, true, true};
    }
    const cirrus::platform::AmplifierDriverContext* contexts[kMaxAmplifiers]{};
    IOReturn ret = kIOReturnSuccess;
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        auto& amp = mAmplifiers[i];
        size_t index = 0;
        while (index < mRuntimeBoard.count && !amp.endpoint.sameDevice(mRuntimeBoard.amplifiers[index].endpoint))
            ++index;
        if (index == mRuntimeBoard.count || !amp.present) {
            ret = kIOReturnNotPermitted;
            break;
        }
        const auto& binding = mRuntimeBoard.amplifiers[index];
        const auto* definition = mDriverRegistry.find(mPlatformProfile->acpiHid, binding.siliconId, amp.endpoint.transport);
        if (!definition) {
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_DEVICE_ID);
            ret = kIOReturnUnsupported;
            break;
        }
        if (amp.endpoint.transport == cirrus::core::BusTransportType::I2C) {
            delete mI2CMaps[i];
            mI2CMaps[i] = nullptr;
            delete mI2CBuses[i];
            mI2CBuses[i] = nullptr;
            amp.registerIO = nullptr;
            mI2CBuses[i] = new cirrus::transport::I2CRegisterBus(mI2CController, {amp.endpoint.i2cAddress, amp.endpoint.tenBitAddress});
            if (!mI2CBuses[i]) {
                ret = kIOReturnNoMemory;
                break;
            }
            mI2CMaps[i] = new cirrus::core::RegisterMap(*mI2CBuses[i], *definition->format(amp.endpoint.transport), true);
            if (!mI2CMaps[i]) {
                ret = kIOReturnNoMemory;
                break;
            }
            amp.registerIO = mI2CMaps[i];
            amp.endpointIndex = uint8_t(i);
        }
        mHostAdapters[i].bind(this, &amp);
        mRuntimePorts[i] = new ServiceRegisterIO(this, amp);
        if (!mRuntimePorts[i]) {
            ret = kIOReturnNoMemory;
            break;
        }
        if (mPlatformProfile->firmwareBundles) {
            if (index >= mPlatformProfile->firmwareBundleCount) {
                ret = kIOReturnNotPermitted;
                break;
            }
            mRuntimeFirmware[i] = mPlatformProfile->firmwareBundles[index];
        } else
            mRuntimeFirmware[i] = {{binding.siliconId, actual.subsystemVendor, actual.subsystemDevice, binding.speakerId,
                                    binding.channelIndex, binding.tuningResource},
                                   cirrus::firmware::FirmwareFormat::Deferred};
        contexts[i] = new cirrus::platform::AmplifierDriverContext{*mRuntimePorts[i],
                                                                   amp.endpoint,
                                                                   mRuntimeBoard,
                                                                   actual,
                                                                   index,
                                                                   mRuntimeFirmware[i],
                                                                   mPlatformProfile->acpiHid,
                                                                   nullptr,
                                                                   nullptr,
                                                                   static_cast<cirrus::platform::AmplifierHost*>(&mHostAdapters[i])};
        if (!contexts[i]) {
            ret = kIOReturnNoMemory;
            break;
        }
    }
    if (ret == kIOReturnSuccess) {
        ret = mAmplifierManager.bind(mDriverRegistry, contexts, mAmplifierCount);
        const auto& failure = mAmplifierManager.bindFailure();
        setProperty("Cirrus_Runtime_Bind_Reason", failure.reason);
        setProperty("Cirrus_Runtime_Bind_Index", uint64_t(uint32_t(failure.index)), 32);
        if (ret != kIOReturnSuccess)
            recordAttachmentFailure("BINDING", failure.reason, ret, uint32_t(failure.index), "AMP_SLOT");
        if (ret != kIOReturnSuccess)
            CIRRUS_ERR("driver binding rejected: index=%lu reason=%s ret=0x%08X", failure.index, failure.reason, ret);
    } else
        setProperty("Cirrus_Runtime_Bind_Reason", "PORT_OR_PROFILE_PREPARATION_FAILED");
    for (auto* context : contexts)
        delete context;
    setProperty("Cirrus_Runtime_Bind_Return", uint64_t(uint32_t(ret)), 32);
    if (ret != kIOReturnSuccess) {
        CIRRUS_ERR("amplifier runtime bind rejected: profile=%s endpoints=%lu ret=0x%08X", mPlatformProfile->name, mAmplifierCount, ret);
        releaseAmplifierDrivers();
        return false;
    }
    setProperty("Cirrus_Runtime", "REGISTERED_DRIVERS");
    return true;
}
void CirrusAudioFixup::releaseAmplifierDrivers() {
    const bool wasBound = mAmplifierManager.bound();
    IOReturn ret = mAmplifierManager.unbind();
    publishRuntimeStatus();
    if (wasBound) {
        mAttachment.amplifier = "DRIVERS_UNBOUND";
        publishAttachmentStatus();
    }
    setProperty("Cirrus_Runtime_Release_Return", uint64_t(uint32_t(ret)), 32);
    if (ret != kIOReturnSuccess)
        CIRRUS_ERR("amplifier runtime teardown incomplete: ret=0x%08X; retained failure evidence", ret);
    for (auto& platform : mHostAdapters)
        platform.clear();
    for (auto*& port : mRuntimePorts) {
        delete port;
        port = nullptr;
    }
    for (size_t i = 0; i < kMaxAmplifiers; ++i) {
        if (mI2CMaps[i])
            mAmplifiers[i].registerIO = nullptr;
        delete mI2CMaps[i];
        mI2CMaps[i] = nullptr;
        delete mI2CBuses[i];
        mI2CBuses[i] = nullptr;
    }
    delete mI2CController;
    mI2CController = nullptr;
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
    if (!mPlatformAuthorized)
        return false;
    const cirrus::platform::ResetControllerQuirk* quirkPtr = mPlatformProfile ? mPlatformProfile->resetQuirk : nullptr;
    if (quirkPtr) {
        const cirrus::platform::ResetControllerQuirk& quirk = *quirkPtr;
        OSDictionary* dict = IOService::nameMatching(quirk.controllerName);
        if (!dict)
            return false;

        OSIterator* iter = IOService::getMatchingServices(dict);
        dict->release();
        if (!iter)
            return false;

        IOService* controller = OSDynamicCast(IOService, iter->getNextObject());
        if (!controller) {
            iter->release();
            return false;
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
    const auto* previousProfile = mPlatformProfile;
    const uint32_t previousSubsystemId = mPlatformSubsystemId;
    const char* acpiHid = nullptr;
    OSObject* ownedHid = nullptr;
    if (auto* acpi = OSDynamicCast(IOACPIPlatformDevice, mProvider)) {
        if (acpi->evaluateObject("_HID", &ownedHid) == kIOReturnSuccess)
            if (auto* hid = OSDynamicCast(OSString, ownedHid))
                acpiHid = hid->getCStringNoCopy();
    }
    if (mProvider && !acpiHid) {
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

    uint32_t pciSubsystemId = 0;
    IOService* controller = getAudioController();
    if (auto* pci = OSDynamicCast(IOPCIDevice, controller))
        pciSubsystemId = (uint32_t(pci->configRead16(kIOPCIConfigSubSystemVendorID)) << 16) | pci->configRead16(kIOPCIConfigSubSystemID);
    if (controller)
        controller->release();
    if (const auto* exact = cirrus::platform::findPlatformProfile(acpiHid, pciSubsystemId))
        mPlatformProfile = exact;
    mPlatformSubsystemId = pciSubsystemId;
    mPlatformAuthorized = cirrus::platform::permitsHardwareInitialization(*mPlatformProfile, pciSubsystemId);
    if (!previousProfile || previousProfile != mPlatformProfile || previousSubsystemId != pciSubsystemId) {
        setProperty("Cirrus_Platform_PCI_SSID", uint64_t(uint32_t(pciSubsystemId)), 32);
        setProperty("Cirrus_Platform_Authorized", mPlatformAuthorized ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Cirrus_Board_Authorized", mPlatformAuthorized ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Cirrus_Platform_Profile", mPlatformProfile->name);
        setProperty("Cirrus_Platform_ACPI_HID", acpiHid ? acpiHid : "unknown");
        CIRRUS_LOG("platform profile selected: %s (hid=%s, endpoints=%lu)", mPlatformProfile->name, acpiHid ? acpiHid : "unknown",
                   mPlatformProfile->endpointCount);
    }
    OSSafeReleaseNULL(ownedHid);
}

// ACPI SPI matching is deliberately opt-in until physical controller tests.
bool CirrusAudioFixup::setupCommunication(IOService* provider) {
    auto* acpi = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!acpi) {
        setProperty("Cirrus_Bus", "I2C");
        const auto* expected =
            mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
                    mPlatformProfile->boardConfiguration->count
                ? mDriverRegistry.find(mPlatformProfile->acpiHid, mPlatformProfile->boardConfiguration->amplifiers[0].siliconId,
                                       cirrus::core::BusTransportType::I2C)
                : mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel,
                                            cirrus::core::BusTransportType::I2C);
        mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
        if (!mI2CController)
            mI2CController = createI2CController();
        if (!mI2CController)
            return false;
        if (!expected) {
            mPlatformAuthorized = false;
            setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
            recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        }
        if (expected && !preflightI2CIdentity(expected))
            return false;
        publishAttachmentStatus();
        return true;
    }
    OSObject* resources = nullptr;
    const IOReturn resourceReturn = acpi->evaluateObject("_CRS", &resources);
    auto* resourceData = OSDynamicCast(OSData, resources);
    cirrus::transport::ACPISPIResources parsed{};
    bool hasI2C = false;
    const auto resourceKind = resourceReturn == kIOReturnSuccess && resourceData
                                  ? cirrus::transport::parseACPISPIResources(static_cast<const uint8_t*>(resourceData->getBytesNoCopy()),
                                                                             resourceData->getLength(), parsed, &hasI2C)
                                  : cirrus::transport::ACPISPIResult::Malformed;
    OSSafeReleaseNULL(resources);
    if (resourceKind == cirrus::transport::ACPISPIResult::NoSPI && hasI2C) {
        setProperty("Cirrus_Bus", "I2C");
        const auto* expected =
            mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
                    mPlatformProfile->boardConfiguration->count
                ? mDriverRegistry.find(mPlatformProfile->acpiHid, mPlatformProfile->boardConfiguration->amplifiers[0].siliconId,
                                       cirrus::core::BusTransportType::I2C)
                : mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel,
                                            cirrus::core::BusTransportType::I2C);
        mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
        if (mPlatformProfile->i2cControllerFactory) {
            if (!mI2CController)
                mI2CController = createI2CController();
            if (!mI2CController)
                return false;
            if (!expected) {
                mPlatformAuthorized = false;
                setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
                recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
            }
            if (expected && !preflightI2CIdentity(expected))
                return false;
            publishAttachmentStatus();
            return true;
        }
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "NATIVE_I2C_PROVIDER_CONTRACT_UNAVAILABLE", kIOReturnUnsupported);
        return false;
    }
    if (resourceKind != cirrus::transport::ACPISPIResult::Success || hasI2C) {
        setProperty("Cirrus_Bus", "ACPI_UNRESOLVED");
        mAttachment.communication = "RESOURCE_REJECTED";
        recordAttachmentFailure("RESOURCE", hasI2C ? "MIXED_I2C_SPI_RESOURCES" : "ACPI_SERIAL_RESOURCE_REJECTED",
                                resourceReturn != kIOReturnSuccess ? resourceReturn : kIOReturnUnsupported);
        return false;
    }
    setProperty("Cirrus_Bus", "SPI");
    if (!bootArgEnabled("-cirrusspi")) {
        setProperty("Cirrus_SPI_Status", "REQUIRES_SPI_OPTION");
        mAttachment.communication = "DISABLED_BY_POLICY";
        recordAttachmentFailure("POLICY", "SPI_OPTION_REQUIRED", kIOReturnNotPermitted);
        CIRRUS_ERR("SPI attachment blocked: -cirrusspi is required");
        return false;
    }
    const cirrus::platform::AmplifierDriverRegistration* selected = nullptr;
    if (mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
        mPlatformProfile->boardConfiguration->count) {
        const auto& first = mPlatformProfile->boardConfiguration->amplifiers[0];
        selected = mDriverRegistry.find(mPlatformProfile->acpiHid, first.siliconId, cirrus::core::BusTransportType::SPI);
    } else
        selected =
            mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel, cirrus::core::BusTransportType::SPI);
    if (!selected) {
        mAttachment.amplifier = "UNSUPPORTED";
        const auto factory =
            mPlatformProfile->spiControllerFactory ? mPlatformProfile->spiControllerFactory : cirrus::transport::createNativeSPIController;
        const IOReturn ret = mNativeSPI.bindControllers(acpi, this, factory);
        mAttachment.communication = ret == kIOReturnSuccess ? "READY" : "FAILED";
        if (ret != kIOReturnSuccess) {
            const auto& failure = mNativeSPI.failure();
            recordAttachmentFailure("COMMUNICATION", failure.reason, ret, uint32_t(failure.index));
            return false;
        }
        mPlatformAuthorized = false;
        setProperty("Cirrus_SPI_Status", "CONTROLLERS_ONLY_AMP_UNSUPPORTED");
        setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
        recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        return true;
    }
    mAttachment.amplifier = "REGISTERED_NOT_PROBED";
    const auto format = *selected->format(cirrus::core::BusTransportType::SPI);
    IOReturn ret = mNativeSPI.bind(acpi, this, format,
                                   mPlatformProfile->spiControllerFactory ? mPlatformProfile->spiControllerFactory
                                                                          : cirrus::transport::createNativeSPIController);
    const IOReturn controllerReturn = ret;
    bool writable = false;
    if (ret == kIOReturnSuccess && mPlatformAuthorized && mPlatformProfile->boardConfiguration) {
        cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId),
                                               mPlatformProfile->acpiHid};
        ret = mNativeSPI.configureAuthorized(mDriverRegistry, *mPlatformProfile->boardConfiguration, actual, mPlatformProfile->acpiHid,
                                             !bootArgEnabled("-cirrusro"));
        writable = ret == kIOReturnSuccess && !bootArgEnabled("-cirrusro");
    }
    mPlatformAuthorized = writable;
    setProperty("Cirrus_Platform_Authorized", writable ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Cirrus_SPI_Bind_Return", uint64_t(uint32_t(ret)), 32);
    setProperty("Cirrus_SPI_Status", ret == kIOReturnSuccess ? (writable ? "BOUND_AUTHORIZED" : "BOUND_READ_ONLY") : "BIND_FAILED");
    const auto& failure = mNativeSPI.failure();
    setProperty("Cirrus_SPI_Failure_Reason", failure.reason);
    setProperty("Cirrus_SPI_Failure_Index", uint64_t(uint32_t(failure.index)), 32);
    setProperty("Cirrus_SPI_Failure_Source", failure.source);
    setProperty("Cirrus_SPI_Failure_Detail", uint64_t(uint32_t(failure.detail)), 32);
    mAttachment.communication = controllerReturn == kIOReturnSuccess ? (writable ? "READY" : "READ_ONLY") : "FAILED";
    publishAttachmentStatus();
    if (ret != kIOReturnSuccess)
        recordAttachmentFailure(controllerReturn == kIOReturnSuccess ? "POLICY" : "COMMUNICATION", failure.reason, ret,
                                uint32_t(failure.index));
    if (ret != kIOReturnSuccess)
        CIRRUS_ERR("SPI bind rejected: reason=%s index=%lu source=%s detail=0x%08X ret=0x%08X", failure.reason, failure.index,
                   failure.source, failure.detail, ret);
    else
        CIRRUS_LOG("SPI bound: endpoints=%lu board=%s access=%s", mNativeSPI.count(), mPlatformProfile->name,
                   writable ? "AUTHORIZED_WRITES" : "READ_ONLY");
    return ret == kIOReturnSuccess;
}

size_t CirrusAudioFixup::detectNativeSPIAmplifiers() {
    mAmplifierCount = 0;
    mProbingAmplifiers = true;
    setProperty("Cirrus_SPI_Resource_Count", uint64_t(uint32_t(mNativeSPI.count())), 32);
    for (size_t i = 0; i < mNativeSPI.count() && i < kMaxAmplifiers; ++i) {
        const auto endpoint = mNativeSPI.endpoint(i);
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_ChipSelect", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiChipSelect)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Controller", i);
        setProperty(key, endpoint.controllerPath);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_ClockHz", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiClockHz)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Mode", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiMode)), 32);
        auto* io = mNativeSPI.registers(i);
        uint32_t identityRegister = 0, identityMask = 0xffffffffU;
        const cirrus::platform::AmplifierDriverRegistration* definition = nullptr;
        const cirrus::platform::AmplifierBinding* configuredBinding = nullptr;
        if (mPlatformProfile->boardConfiguration) {
            const auto& board = *mPlatformProfile->boardConfiguration;
            for (size_t j = 0; j < board.count; ++j)
                if (endpoint.sameDevice(board.amplifiers[j].endpoint)) {
                    configuredBinding = &board.amplifiers[j];
                    const char* hid = configuredBinding->acpiHid ? configuredBinding->acpiHid : mPlatformProfile->acpiHid;
                    if (hid && mPlatformProfile->acpiHid && !strcmp(hid, mPlatformProfile->acpiHid))
                        definition = mDriverRegistry.find(hid, configuredBinding->siliconId, cirrus::core::BusTransportType::SPI);
                    break;
                }
            if (!definition) {
                mAttachment.amplifier = "UNSUPPORTED";
                recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported, uint32_t(i));
                CIRRUS_ERR("SPI endpoint has no matching registered driver: endpoint=%lu controller=%s CS=%u; no probe", i,
                           endpoint.controllerPath, endpoint.spiChipSelect);
                continue;
            }
        } else
            definition =
                mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel, cirrus::core::BusTransportType::SPI);
        if (!definition)
            continue;
        identityRegister = definition->identityRegister;
        identityMask = definition->identityMask;
        uint32_t id = 0;
        const bool readable = io && io->read(identityRegister, &id);
        const uint32_t rawId = id;
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, identityRegister, readable ? rawId : 0,
                    io ? io->lastError() : kIOReturnNotReady);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_RawDeviceID", i);
        setProperty(key, uint64_t(uint32_t(rawId)), 32);
        id &= identityMask;
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(io ? io->lastError() : kIOReturnNotReady)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable) {
            mAttachment.communication = "FAILED";
            recordAttachmentFailure("COMMUNICATION", "SPI_IDENTITY_TRANSFER_FAILED", io ? io->lastError() : kIOReturnNotReady, uint32_t(i));
            CIRRUS_ERR("SPI identity read failed: endpoint=%lu controller=%s CS=%u reg=0x%08X ret=0x%08X", i, endpoint.controllerPath,
                       endpoint.spiChipSelect, identityRegister, io ? io->lastError() : kIOReturnNotReady);
            continue;
        }
        // Both manifest and legacy discovery use module-owned identity policy.
        if (id != (configuredBinding ? configuredBinding->siliconId : definition->siliconId)) {
            mAttachment.communication = "READY";
            mAttachment.amplifier = "IDENTITY_MISMATCH";
            recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            CIRRUS_ERR("SPI silicon unsupported/mismatched: endpoint=%lu controller=%s CS=%u DEVID=0x%08X; no initialization", i,
                       endpoint.controllerPath, endpoint.spiChipSelect, id);
            continue;
        }
        auto& amp = mAmplifiers[mAmplifierCount++];
        amp = {};
        snprintf(amp.endpointLabel, sizeof(amp.endpointLabel), "spi%lu", i);
        amp.name = amp.endpointLabel;
        amp.address = 0xff; // SPI chip-select is never an I2C slave address.
        amp.channel = cirrus::core::AudioChannel::MaxChannels;
        amp.registerIO = io;
        amp.endpoint = endpoint;
        amp.endpointIndex = uint8_t(i);
        amp.model = definition->model;
        amp.present = true;
        amp.deviceId = id;
        if (mPlatformProfile->boardConfiguration) {
            const auto& board = *mPlatformProfile->boardConfiguration;
            for (size_t j = 0; j < board.count; ++j) {
                const auto& binding = board.amplifiers[j];
                if (endpoint.sameDevice(binding.endpoint) && id == binding.siliconId)
                    amp.channel = static_cast<cirrus::core::AudioChannel>(binding.channelIndex);
            }
        }
        CIRRUS_LOG("SPI endpoint=%lu controller=%s CS=%u clock=%u mode=%u DEVID=0x%08X", i, endpoint.controllerPath, endpoint.spiChipSelect,
                   endpoint.spiClockHz, endpoint.spiMode, id);
    }
    mProbingAmplifiers = false;
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? (mPlatformAuthorized ? "DETECTED_AUTHORIZED" : "DETECTED_READ_ONLY") : "NONE");
    return mAmplifierCount;
}

void CirrusAudioFixup::teardownCommunication() {
    releaseAmplifierDrivers();
    for (auto& amp : mAmplifiers)
        amp.registerIO = nullptr;
    mNativeSPI.shutdown();
}

// Validate diagnostic options before any reset or amplifier write.
// Provider discovery happens before service publication; later playback and
// power transitions run on the service workloop.
bool CirrusAudioFixup::start(IOService* provider) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    gCirrusDebug = cirrus::support::kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg");

    setProperty("Author", "Tran Kinh Hoang (hoaugtr)");
    setProperty("Cirrus_Build_Configuration", cirrus::support::kBuildConfiguration);
    setProperty("Cirrus_Debug_Enabled", gCirrusDebug ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Cirrus_Audio_Event_Mode", "HDA_TIMER_FALLBACK");
    IOLog("CirrusAudioFixup: %s build; verbose logging %s\n", cirrus::support::kBuildConfiguration, gCirrusDebug ? "enabled" : "disabled");
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("Author: Tran Kinh Hoang (hoaugtr)");
    CIRRUS_LOG("start");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
        return false;
    }

    mProvider = provider;
    logProviderInfo(provider);
    dumpProviderProperties(provider);
    resolvePlatformProfile();

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

    if (!setupCommunication(provider)) {
        teardownCommunication();
        mProvider = nullptr;
        super::stop(provider);
        return false;
    }
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
        bool hardwareResetVerified = performPlatformHardwareReset();
        if (!hardwareResetVerified) {
            CIRRUS_LOG("Platform hardware reset not asserted or verified; continuing with ACPI and I2C soft reset");
        }

        IOSleep(15);
        if (mNativeSPI.bound())
            detectNativeSPIAmplifiers();
        else
            detectAmplifiers();
    }

    if (!setupProbeTimer()) {
        CIRRUS_ERR("probe timer setup failed");
        teardownCommunication();
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
        if (!mPlatformIdentityPending)
            bringUpAmplifiers();
        if (mProbeTimer) {
            mProbeTimer->setTimeoutMS(50);
        }
    }

    if (!setupPowerManagement(provider)) {
        stop(provider);
        return false;
    }
    setupAmplifierInterrupt();
    registerService();
    return true;
}

// Initialize each supported amplifier into verified, muted idle.
// Failed CS35L41 stages attempt output cleanup and a DSP halt before returning.
// Unsupported models are rejected without silicon-specific writes.
// Keep the original failure in diagnostics even if cleanup also fails.
// A debug phase stops at its boundary and never publishes playback readiness.
void CirrusAudioFixup::bringUpAmplifiers() {
    CIRRUS_LOG("starting hardware initialization flow");
    if (bootArgEnabled("-cirrusro"))
        return;
    if (!mPlatformAuthorized || !mPlatformProfile || mAmplifierCount != expectedAmplifierCount()) {
        setProperty("Cirrus_Runtime_Block_Reason", !mPlatformAuthorized ? "BOARD_NOT_AUTHORIZED" : "INCOMPLETE_TOPOLOGY");
        CIRRUS_ERR("initialization blocked: authorized=%u detected=%lu expected=%lu; no amplifier initialization",
                   unsigned(mPlatformAuthorized), mAmplifierCount, expectedAmplifierCount());
        publishDriverVerdict();
        return;
    }
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        if (!mAmplifiers[i].present) {
            publishDriverVerdict();
            return;
        }
    }

    if (stopAfterDebugStage("probe")) {
        publishDriverVerdict();
        return;
    }
    if (!bindAmplifierDrivers()) {
        publishDriverVerdict();
        return;
    }
    if (mAmplifierManager.stage() == cirrus::core::AmplifierStage::Detached) {
        IOReturn result = mAmplifierManager.bringUp();
        setProperty("Cirrus_Runtime_BringUp_Return", uint64_t(uint32_t(result)), 32);
        if (result != kIOReturnSuccess)
            CIRRUS_LOG("amplifier bring-up paused/failed: stage=%u ret=0x%08X", unsigned(mAmplifierManager.stage()), result);
    }
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        const auto* status = mAmplifierManager.status(i);
        if (status) {
            mAmplifiers[i].initialized =
                status->stage == cirrus::core::AmplifierStage::Ready || status->stage == cirrus::core::AmplifierStage::Playing;
            if (status->stage == cirrus::core::AmplifierStage::Faulted)
                mAmplifiers[i].playbackFaulted = true;
            if (status->firstFailure != kIOReturnSuccess)
                CIRRUS_ERR("AMP driver failure: index=%lu stage=%u first=0x%08X cleanup=0x%08X", i, unsigned(status->firstFailureStage),
                           status->firstFailure, status->cleanupResult);
        }
    }

    publishDriverVerdict();
    CIRRUS_LOG("hardware initialization flow complete");
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
    const IOReturn result = registerPowerDriver(this, mPowerStates, 2);
    setProperty("Cirrus_PM_Registration_Return", uint64_t(uint32_t(result)), 32);
    if (result != kIOReturnSuccess)
        CIRRUS_ERR("PM_REGISTRATION_FAILED operation=registerPowerDriver return=0x%08X; service will stop", result);
    return result == kIOReturnSuccess;
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
        mPowerAvailable = false;
        clearHdaCache();
        mHdaState.streamActive = mHdaState.observed = mHdaState.converterPrepared = false;
        mHdaState.lastDescriptor = 0xFF;
        mHdaState.lastStreamTag = 0;
        mHdaState.lastFormat = 0;
        setProperty("Cirrus_PM_Powered", uint64_t(uint32_t(0)), 32);
    } else {
        if (!resumeCommunication()) {
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

// Finish hardware cleanup while the gate and workloop still exist.
// Cancel and remove event sources before releasing their owners; a timer
// callback must never outlive the provider pointer it uses.
void CirrusAudioFixup::stop(IOService* provider) {
    teardownAmplifierInterrupt();
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

void CirrusAudioFixup::logProviderInfo(IOService* provider) {
    if (!provider) {
        CIRRUS_ERR("provider is null");
        return;
    }

    const auto cpu = cirrus::support::readCPUIdentity();
    setProperty("Cirrus_CPU_Vendor", cirrus::support::cpuVendorName(cpu.vendor));
    setProperty("Cirrus_CPU_Vendor_ID", cpu.vendor);
    setProperty("Cirrus_CPU_Brand", cpu.brand);
    if (auto* root = IORegistryEntry::fromPath("IODeviceTree:/")) {
        if (auto* data = OSDynamicCast(OSData, root->getProperty("model"))) {
            char model[128]{};
            const size_t length = data->getLength() < sizeof(model) - 1 ? data->getLength() : sizeof(model) - 1;
            if (length && data->getBytesNoCopy()) {
                memcpy(model, data->getBytesNoCopy(), length);
                setProperty("Cirrus_Machine_Model", model);
            }
        }
        root->release();
    }
    setProperty("Cirrus_Provider_Name", provider->getName());
    setProperty("Cirrus_Provider_Class", provider->getMetaClass()->getClassName());
    char providerPath[256]{};
    int pathLength = sizeof(providerPath);
    if (provider->getPath(providerPath, &pathLength, gIOServicePlane))
        setProperty("Cirrus_Provider_Path", providerPath);
    IOLog("CirrusAudioFixup: ATTACH cpu=%s brand=%s provider=%s class=%s path=%s\n", cirrus::support::cpuVendorName(cpu.vendor), cpu.brand,
          provider->getName(), provider->getMetaClass()->getClassName(), providerPath);
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
    if (!gCirrusDebug)
        return;
    if (!provider) {
        return;
    }

    CIRRUS_LOG("provider properties follow");

    OSDictionary* properties = provider->getPropertyTable();
    if (!properties)
        return;
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

// The reference Linux capture uses a shared GPIO84, active-low open-drain
// GPIO2 on both amps. Keep this additional delivery path opt-in until its
// registration, interrupt delivery and suspend ordering are tested on macOS.
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
    mWorkLoop = getWorkLoop();
    if (!mWorkLoop) {
        mWorkLoop = IOWorkLoop::workLoop();
    } else {
        mWorkLoop->retain();
    }

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
size_t CirrusAudioFixup::detectConfiguredI2CAmplifiers() {
    // Live drivers borrow endpoint state/ports. Rediscovery must not replace
    // them; an unbound rescan releases old discovery ports before opening anew.
    if (mAmplifierManager.bound())
        return mAmplifierCount;
    releaseAmplifierDrivers();
    mAmplifierCount = 0;
    if (!mPlatformAuthorized || !mPlatformProfile || !mPlatformProfile->boardConfiguration) {
        recordAttachmentFailure("POLICY", "BOARD_NOT_AUTHORIZED", kIOReturnNotPermitted);
        return 0;
    }
    const auto& board = *mPlatformProfile->boardConfiguration;
    if (!board.amplifiers) {
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return 0;
    }
    const auto* expected =
        board.count ? mDriverRegistry.find(mPlatformProfile->acpiHid, board.amplifiers[0].siliconId, cirrus::core::BusTransportType::I2C)
                    : nullptr;
    mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
    int length = sizeof(mRuntimeControllerPath);
    if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane))
        return 0;
    const bool readOnly = bootArgEnabled("-cirrusro");
    // Retain the same communication owner from read-only identity discovery
    // through runtime binding; do not double-open a provider/controller.
    if (!mI2CController)
        mI2CController = createI2CController();
    if (!mI2CController)
        return 0;
    for (size_t i = 0; i < board.count && mAmplifierCount < kMaxAmplifiers; ++i) {
        const auto& binding = board.amplifiers[i];
        const auto& endpoint = binding.endpoint;
        if (!endpoint.valid() || endpoint.transport != cirrus::core::BusTransportType::I2C || endpoint.tenBitAddress ||
            strcmp(endpoint.controllerPath, mRuntimeControllerPath)) {
            recordAttachmentFailure("RESOURCE", "I2C_ENDPOINT_UNMATCHED", kIOReturnUnsupported, uint32_t(i));
            CIRRUS_ERR("I2C endpoint unsupported/unmatched: index=%lu controller=%s address=0x%X tenBit=%u bus=%u", i,
                       endpoint.controllerPath, unsigned(endpoint.i2cAddress), unsigned(endpoint.tenBitAddress),
                       unsigned(endpoint.transport));
            continue;
        }
        const char* hid = binding.acpiHid ? binding.acpiHid : mPlatformProfile->acpiHid;
        const auto* driver = mDriverRegistry.find(hid, binding.siliconId, cirrus::core::BusTransportType::I2C);
        if (!hid || !mPlatformProfile->acpiHid || !driver || strcmp(hid, mPlatformProfile->acpiHid)) {
            mAttachment.amplifier = "UNSUPPORTED";
            recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported, uint32_t(i));
            CIRRUS_ERR("I2C driver unsupported: endpoint=%lu hid=%s silicon=0x%08X", i, hid, binding.siliconId);
            continue;
        }
        cirrus::transport::I2CRegisterBus bus(mI2CController, {endpoint.i2cAddress, endpoint.tenBitAddress});
        cirrus::core::RegisterMap map(bus, *driver->format(cirrus::core::BusTransportType::I2C), false);
        uint32_t id = 0;
        bool readable = map.read(driver->identityRegister, &id);
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, driver->identityRegister, readable ? id : 0, map.lastError());
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(map.lastError())), 32);
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable) {
            mAttachment.communication = map.lastError() == kIOReturnUnsupported ? "UNSUPPORTED" : "FAILED";
            recordAttachmentFailure("COMMUNICATION", "I2C_IDENTITY_TRANSFER_FAILED", map.lastError(), uint32_t(i));
        } else
            mAttachment.communication = "READY";
        if (!readable || (id & driver->identityMask) != binding.siliconId) {
            if (readable) {
                mAttachment.amplifier = "IDENTITY_MISMATCH";
                recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            }
            CIRRUS_ERR(
                "I2C identity rejected: endpoint=%lu controller=%s address=0x%X actual=0x%08X expected=0x%08X mask=0x%08X ret=0x%08X", i,
                endpoint.controllerPath, unsigned(endpoint.i2cAddress), id, binding.siliconId, driver->identityMask, map.lastError());
            continue;
        }
        auto& amp = mAmplifiers[mAmplifierCount];
        amp = {};
        snprintf(amp.endpointLabel, sizeof(amp.endpointLabel), "i2c%lu", i);
        amp.name = amp.endpointLabel;
        amp.endpoint = endpoint;
        amp.address = uint8_t(endpoint.i2cAddress);
        amp.endpointIndex = uint8_t(mAmplifierCount);
        amp.channel = static_cast<cirrus::core::AudioChannel>(binding.channelIndex);
        if (readOnly) {
            mI2CBuses[mAmplifierCount] =
                new cirrus::transport::I2CRegisterBus(mI2CController, {endpoint.i2cAddress, endpoint.tenBitAddress});
            if (!mI2CBuses[mAmplifierCount])
                break;
            mI2CMaps[mAmplifierCount] =
                new cirrus::core::RegisterMap(*mI2CBuses[mAmplifierCount], *driver->format(cirrus::core::BusTransportType::I2C), false);
            if (!mI2CMaps[mAmplifierCount])
                break;
            amp.registerIO = mI2CMaps[mAmplifierCount];
        }
        amp.model = driver->model;
        amp.deviceId = binding.siliconId;
        amp.present = true;
        ++mAmplifierCount;
    }
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? "DETECTED_CONFIGURED_I2C" : "NONE");
    return mAmplifierCount;
}

size_t CirrusAudioFixup::detectAmplifiers() {
    if (mAmplifierManager.bound())
        return mAmplifierCount;
    if (mPlatformProfile && mPlatformProfile->boardConfiguration)
        return detectConfiguredI2CAmplifiers();
    for (size_t i = 0; i < kMaxAmplifiers; ++i) {
        delete mI2CMaps[i];
        mI2CMaps[i] = nullptr;
        delete mI2CBuses[i];
        mI2CBuses[i] = nullptr;
        mAmplifiers[i] = {};
    }
    mAmplifierCount = 0;
    const auto& profile = mPlatformProfile ? *mPlatformProfile : cirrus::platform::defaultPlatformProfile();
    const auto* definition = mDriverRegistry.findModel(profile.acpiHid, profile.amplifierModel, cirrus::core::BusTransportType::I2C);
    if (!definition) {
        mAttachment.amplifier = "UNSUPPORTED";
        recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        return 0;
    }
    mAttachment.amplifier = "REGISTERED_NOT_PROBED";
    if (!profile.endpoints || !profile.endpointCount) {
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return 0;
    }
    if (!mPlatformAuthorized && !bootArgEnabled("-cirruslegacyprobe")) {
        recordAttachmentFailure("POLICY", "BOARD_NOT_AUTHORIZED", kIOReturnNotPermitted);
        return 0;
    }
    int pathLength = sizeof(mRuntimeControllerPath);
    if (!mProvider->getPath(mRuntimeControllerPath, &pathLength, gIOServicePlane))
        return 0;
    if (!mI2CController)
        mI2CController = createI2CController();
    if (!mI2CController)
        return 0;
    mAmplifierCount = 0;
    mProbingAmplifiers = true;
    for (size_t i = 0; i < profile.endpointCount && mAmplifierCount < kMaxAmplifiers; ++i) {
        const auto& candidate = profile.endpoints[i];
        const size_t index = mAmplifierCount;
        auto* bus = new cirrus::transport::I2CRegisterBus(mI2CController, {candidate.address, false});
        if (!bus)
            break;
        auto* map = new cirrus::core::RegisterMap(*bus, *definition->format(cirrus::core::BusTransportType::I2C), false);
        if (!map) {
            delete bus;
            break;
        }
        uint32_t id = 0;
        const bool readable = map->read(definition->identityRegister, &id);
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, definition->identityRegister, readable ? id : 0, map->lastError());
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(map->lastError())), 32);
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable || (id & definition->identityMask) != definition->siliconId) {
            if (!readable) {
                mAttachment.communication = map->lastError() == kIOReturnUnsupported ? "UNSUPPORTED" : "FAILED";
                recordAttachmentFailure("COMMUNICATION", "I2C_IDENTITY_TRANSFER_FAILED", map->lastError(), uint32_t(i));
            } else {
                mAttachment.communication = "READY";
                mAttachment.amplifier = "IDENTITY_MISMATCH";
                recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            }
            delete map;
            delete bus;
            continue;
        }
        auto& amp = mAmplifiers[index];
        amp = {};
        amp.name = candidate.name;
        amp.address = candidate.address;
        amp.model = definition->model;
        amp.channel = candidate.channel;
        amp.present = true;
        amp.deviceId = id;
        amp.endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, candidate.address};
        amp.endpointIndex = uint8_t(index);
        amp.registerIO = map;
        mI2CMaps[index] = map;
        mI2CBuses[index] = bus;
        ++mAmplifierCount;
    }
    mProbingAmplifiers = false;
    setProperty("Cirrus_Discovery_Fallback", kOSBooleanFalse);
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? "DETECTED" : "NONE");
    return mAmplifierCount;
}

void CirrusAudioFixup::runReadOnlyProbe() {
    CIRRUS_LOG("read-only probe begin");

    for (size_t i = 0; i < mAmplifierCount; ++i) {
        probeAmplifier(mAmplifiers[i]);
    }

    publishStatistics();

    if (bootArgEnabled("-cirrusdumptrace")) {
        dumpTraceBuffer();
    }

    CIRRUS_LOG("read-only probe complete");
}

bool CirrusAudioFixup::supportedHdaFormat(uint16_t format) { return cirrus::platform::hda::HDAStreamWatcher::isFormatSupported(format); }

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

// Missing board identity is a bounded boot dependency, not an unsupported board.
// Revalidate existing authorization before any reset or AMP write; known failures
// remain terminal. Calls are serialized on the service workloop.
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
    if (!mPlatformAuthorized) {
        publishDriverVerdict();
        return true;
    }
    performPlatformHardwareReset();
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

// Poll driver-owned pending resources. Only Waiting lifecycles can retry;
// silicon faults remain latched and peers retain the shared rollback policy.
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
    const bool routePermitsPlayback = !eventRoute || (mAudioState.speakers && (!mAudioState.engineKnown || mAudioState.engineRunning));
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
            return;
        }
        mProbeTimer->setTimeoutMS(mHdaState.streamActive ? 100 : 50);
    }
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
    const auto stats = mTraceStats;
    IOLockUnlock(mTraceLock);
    setProperty("Cirrus_Read_Success", stats.readSuccess, 32);
    setProperty("Cirrus_Read_Fail", stats.readFail, 32);
    setProperty("Cirrus_Write_Success", stats.writeSuccess, 32);
    setProperty("Cirrus_Write_Fail", stats.writeFail, 32);
    setProperty("Cirrus_Bulk_Success", stats.bulkSuccess, 32);
    setProperty("Cirrus_Bulk_Fail", stats.bulkFail, 32);
    setProperty("Cirrus_Offline_Return_Count", stats.noackCount, 32);
    setProperty("Cirrus_Timeout_Return_Count", stats.retries, 32);
    // Retain the legacy property for collectors; Offline is not electrical NACK proof.
    setProperty("Cirrus_NOACK_Count", stats.noackCount, 32);
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

const char* CirrusAudioFixup::failureName(DiagnosticFailure failure) { return cirrus::diagnostics::failureName(failure); }

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
    if (first) {
        diag.firstFailure = failure;
        diag.firstStage = diag.stage;
        diag.firstReg = reg;
        diag.firstExpected = expected;
        diag.firstActual = actual;
        diag.firstIOReturn = ioReturn;
        char key[96];
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstStage_%s", amp.name);
        setProperty(key, stageName(diag.firstStage));
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstReg_%s", amp.name);
        setProperty(key, reg, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstExpected_%s", amp.name);
        setProperty(key, expected, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstActual_%s", amp.name);
        setProperty(key, actual, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstIOReturn_%s", amp.name);
        setProperty(key, uint64_t(uint32_t(ioReturn)), 32);
    }
    diag.latestFailure = failure;
    if (diag.failureCount != UINT32_MAX)
        ++diag.failureCount;
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
        setProperty(property, uint64_t(uint32_t(ioReturn)), 32);
        publishStatistics();
    }

    if (first || changed) {
        CIRRUS_ERR("DIAG_FAIL amp=%s stage=%s code=%s count=%u reg=0x%08X expected=0x%08X actual=0x%08X io=0x%08X first=%s", amp.name,
                   stageName(diag.stage), failureName(failure), diag.failureCount, reg, expected, actual, ioReturn,
                   failureName(diag.firstFailure));
    }

    if (gCirrusDebug && !mCapturingFailureSnapshot && (first || (captureSnapshot && diag.snapshotFailure != failure))) {
        char latestTrace[80], firstTrace[80];
        snprintf(latestTrace, sizeof(latestTrace), "Cirrus_Trace_Latest_%s", amp.name);
        snprintf(firstTrace, sizeof(firstTrace), "Cirrus_Trace_First_%s", amp.name);
        // Freeze transfer history before snapshot reads add diagnostic traffic.
        dumpTraceBuffer(latestTrace, first ? firstTrace : nullptr);
        if (captureSnapshot && diag.snapshotFailure != failure) {
            diag.snapshotFailure = failure;
            captureFailureSnapshot(amp, failure);
        }
    }
}

// Publish only lifecycle transitions. Do not allocate/log at steady-state polls.
// Removed slots are marked unbound while retaining their last failure evidence.
bool CirrusAudioFixup::publishRuntimeStatus() {
    const uint32_t count = uint32_t(mAmplifierManager.count());
    const uint32_t stage = uint32_t(mAmplifierManager.stage());
    bool changed = count != mPublishedRuntimeCount || stage != mPublishedRuntimeStage;
    const uint32_t previousCount = mPublishedRuntimeCount == UINT32_MAX ? 0 : mPublishedRuntimeCount;
    if (changed) {
        setProperty("Cirrus_Runtime_Stage", uint64_t(stage), 32);
        setProperty("Cirrus_Runtime_Count", uint64_t(count), 32);
        mPublishedRuntimeCount = count;
        mPublishedRuntimeStage = stage;
    }
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        const auto* status = mAmplifierManager.status(i);
        if (!status)
            continue;
        cirrus::diagnostics::RuntimeDiagnosticSnapshot next{uint32_t(status->stage),         uint32_t(status->firstFailureStage),
                                                            uint32_t(status->firstFailure),  uint32_t(status->latestFailure),
                                                            uint32_t(status->cleanupResult), status->failures};
        if (!mRuntimePublished[i].update(next))
            continue;
        changed = true;
        char key[64]{};
        const char* names[] = {"Stage", "FailureStage", "FirstFailure", "LatestFailure", "CleanupReturn", "FailureCount"};
        const uint32_t values[] = {next.stage, next.failureStage, next.firstFailure, next.latestFailure, next.cleanup, next.failures};
        for (size_t field = 0; field < 6; ++field) {
            snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_%s", i, names[field]);
            setProperty(key, uint64_t(values[field]), 32);
        }
        snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Bound", i);
        setProperty(key, uint64_t(1), 32);
    }
    for (size_t i = count; i < previousCount; ++i) {
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Bound", i);
        setProperty(key, uint64_t(0), 32);
        if (const auto* released = mAmplifierManager.releasedStatus(i)) {
            const char* names[] = {"Stage", "FailureStage", "FirstFailure", "LatestFailure", "CleanupReturn", "FailureCount"};
            const uint32_t values[] = {uint32_t(released->stage),         uint32_t(released->firstFailureStage),
                                       uint32_t(released->firstFailure),  uint32_t(released->latestFailure),
                                       uint32_t(released->cleanupResult), released->failures};
            for (size_t field = 0; field < 6; ++field) {
                snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Released_%s", i, names[field]);
                setProperty(key, uint64_t(values[field]), 32);
            }
        }
        mRuntimePublished[i] = {};
    }
    if (changed)
        publishStatistics();
    return changed;
}

// Publish initialization state, not an acoustic pass/fail result.
// A fault takes precedence over a deliberate debug halt. READY_DSP describes
// the driver's verified state; speaker routing remains a separate check.
void CirrusAudioFixup::publishDriverVerdict() {
    publishRuntimeStatus();
    if (mPlatformIdentityPending) {
        setProperty("Cirrus_Driver_Verdict", "WAITING_BOARD_IDENTITY");
        return;
    }
    using cirrus::core::AmplifierStage;
    switch (mAmplifierManager.stage()) {
    case AmplifierStage::Ready:
        mAttachment.amplifier = "READY";
        break;
    case AmplifierStage::Playing:
        mAttachment.amplifier = "PLAYING";
        break;
    case AmplifierStage::Faulted:
        mAttachment.amplifier = "FAULTED";
        break;
    case AmplifierStage::Waiting:
        mAttachment.amplifier = "WAITING_RESOURCES";
        break;
    case AmplifierStage::DebugHalted:
        mAttachment.amplifier = "DEBUG_HALTED";
        break;
    case AmplifierStage::Suspended:
        mAttachment.amplifier = "SUSPENDED";
        break;
    default:
        break;
    }
    publishAttachmentStatus();
    bool topologyComplete = mPlatformProfile && mAmplifierCount == expectedAmplifierCount();
    for (size_t i = 0; i < mAmplifierCount; ++i)
        topologyComplete &= mAmplifiers[i].present;
    bool allInitialized = (mAmplifierCount > 0);
    bool anyFaulted = false, identityPending = false;
    const char* readyVerdict = nullptr;
    const char* waitingVerdict = "WAITING_FOR_RESOURCES";
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        const auto report = mAmplifierManager.report(i);
        const auto* status = mAmplifierManager.status(i);
        const bool ready =
            status && (status->stage == cirrus::core::AmplifierStage::Ready || status->stage == cirrus::core::AmplifierStage::Playing);
        allInitialized &= ready && (!report.managed || report.initialized);
        anyFaulted |= mAmplifiers[i].playbackFaulted || report.faulted;
        identityPending |= report.waiting;
        if (report.waitingVerdict)
            waitingVerdict = report.waitingVerdict;
        const char* label = report.readyVerdict ? report.readyVerdict : "READY_DRIVERS";
        if (!readyVerdict)
            readyVerdict = label;
        else if (strcmp(readyVerdict, label))
            readyVerdict = "READY_DRIVERS";
    }
    anyFaulted |= mAmplifierManager.stage() == cirrus::core::AmplifierStage::Faulted;
    if (anyFaulted)
        mAttachment.amplifier = "FAULTED";
    publishAttachmentStatus();
    const char* verdict = !mPowerAvailable              ? "SUSPENDED"
                          : bootArgEnabled("-cirrusro") ? "READ_ONLY"
                          : !mPlatformAuthorized        ? "UNSUPPORTED_PLATFORM"
                          : anyFaulted                  ? "FAULT_LATCHED"
                          : !topologyComplete           ? "INCOMPLETE_TOPOLOGY"
                          : identityPending             ? waitingVerdict
                          : mDebugPhaseHalted           ? "DEBUG_PHASE_HALTED"
                          : !allInitialized             ? "FAILED_INIT"
                          : readyVerdict                ? readyVerdict
                                                        : "READY_DRIVERS";
    OSString* value = OSString::withCString(verdict);
    if (value) {
        setProperty("Cirrus_Driver_Verdict", value);
        value->release();
    }
    for (size_t i = 0; i < mAmplifierCount; ++i)
        CIRRUS_LOG("DIAG_VERDICT driver=%s amp=%s first=%s latest=%s last_good=%s", verdict, mAmplifiers[i].name,
                   failureName(mAmplifiers[i].diagnostic.firstFailure), failureName(mAmplifiers[i].diagnostic.latestFailure),
                   stageName(mAmplifiers[i].diagnostic.lastGoodStage));
}

void CirrusAudioFixup::dumpTraceBuffer(const char* propertyName, const char* mirrorPropertyName) {
    if (!mTraceLock)
        return;

    CIRRUS_LOG("--- TRACE BUFFER DUMP START ---");

    size_t bufferSize = 128 * 1024;
    char* dumpBuffer = (char*)IOMallocData(bufferSize);
    if (!dumpBuffer) {
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_text return=NO_MEMORY");
        return;
    }
    auto* entries = static_cast<TraceEntry*>(IOMallocData(sizeof(TraceEntry) * kTraceBufferSize));
    if (!entries) {
        IOFreeData(dumpBuffer, bufferSize);
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_snapshot return=NO_MEMORY");
        return;
    }
    // Copy a bounded history while locked; allocate, format and publish outside
    // the transfer recorder lock. Diagnostic output must not serialize bus work.
    size_t count = 0;
    IOLockLock(mTraceLock);
    uint32_t cursor = mTraceHead;
    while (cursor != mTraceTail && count < kTraceBufferSize) {
        entries[count++] = mTraceBuffer[cursor];
        cursor = (cursor + 1) % kTraceBufferSize;
    }
    IOLockUnlock(mTraceLock);
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;
    char lineBuffer[128];

    for (size_t i = 0; i < count; ++i) {
        const TraceEntry& e = entries[i];
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

        char nativeLabel[16]{};
        snprintf(nativeLabel, sizeof(nativeLabel), "AMP%u", e.amp);
        const char* ampStr = nativeLabel;

        if (e.isBulk) {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] BULK %s 0x%05X len=%u ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.value, e.ret);
        } else {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] %s 0x%05X %s 0x%08X ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.isWrite ? "<-" : "->", e.value, e.ret);
        }

        size_t lineLen = strlen(lineBuffer);
        if (currentLen + lineLen < bufferSize - 1) {
            memcpy(dumpBuffer + currentLen, lineBuffer, lineLen);
            currentLen += lineLen;
            dumpBuffer[currentLen] = 0;
        }
    }
    CIRRUS_LOG("--- TRACE BUFFER DUMP END ---");

    OSString* strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propertyName ? propertyName : "Cirrus_Trace_Dump", strObj);
        if (mirrorPropertyName)
            setProperty(mirrorPropertyName, strObj);
        strObj->release();
    } else {
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_property return=NO_MEMORY");
    }
    IOFreeData(dumpBuffer, bufferSize);

    IOFreeData(entries, sizeof(TraceEntry) * kTraceBufferSize);
}

bool CirrusAudioFixup::bulkRead(AmplifierState& amp, UInt32 reg, UInt8* data, size_t length, TraceSource source) {
    if (!data || !length) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && (source == TRACE_DUMP ? amp.registerIO->bulkReadForDiagnostic(reg, data, length)
                                                   : amp.registerIO->bulkRead(reg, data, length));
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, false, true, reg, uint32_t(length), result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, uint32_t(length), 0, result);
    mLastTransferReturn = result;
    return ok;
}
bool CirrusAudioFixup::bulkWrite(AmplifierState& amp, UInt32 reg, const UInt8* data, size_t length, TraceSource source) {
    if (length && !data) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && amp.registerIO->bulkWrite(reg, data, length);
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, true, true, reg, uint32_t(length), result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, uint32_t(length), 0, result);
    mLastTransferReturn = result;
    return ok;
}
bool CirrusAudioFixup::readRegister(AmplifierState& amp, UInt32 reg, UInt32* value, TraceSource source) {
    if (!value) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && (source == TRACE_DUMP ? amp.registerIO->readForDiagnostic(reg, value) : amp.registerIO->read(reg, value));
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, false, false, reg, ok ? *value : 0, result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, 4, 0, result);
    mLastTransferReturn = result;
    return ok;
}
bool CirrusAudioFixup::writeRegister(AmplifierState& amp, UInt32 reg, UInt32 value, TraceSource source) {
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && amp.registerIO->write(reg, value);
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, true, false, reg, value, result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, value, 0, result);
    mLastTransferReturn = result;
    return ok;
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

    return false;
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

IOService* CirrusAudioFixup::getAudioController() { return audioController(); }

void CirrusAudioFixup::captureFailureSnapshot(AmplifierState& amp, DiagnosticFailure) {
    for (size_t i = 0; i < mAmplifierCount; ++i)
        if (&mAmplifiers[i] == &amp) {
            mAmplifierManager.captureDiagnostics(i);
            return;
        }
}
void CirrusAudioFixup::probeAmplifier(AmplifierState& amp) {
    const auto* definition = mDriverRegistry.find(mPlatformProfile->acpiHid, amp.deviceId, amp.endpoint.transport);
    if (!definition || !definition->inspect)
        return;
    AmplifierHostAdapter host;
    host.bind(this, &amp);
    ServiceRegisterIO io(this, amp, TRACE_DUMP);
    cirrus::platform::AmplifierBinding binding{
        amp.endpoint, amp.deviceId, uint32_t(amp.channel), 1, amp.name, mPlatformProfile->acpiHid, 2};
    cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid};
    cirrus::platform::BoardConfiguration board{actual, &binding, 1, false, false, false};
    cirrus::firmware::FirmwareBundle firmware{};
    cirrus::platform::AmplifierDriverContext context{io,      amp.endpoint, board, actual, 0, firmware, mPlatformProfile->acpiHid,
                                                     nullptr, nullptr,      &host};
    definition->inspect(context);
}

// Probe a known, non-destructive identity before any board reset. An absent
// device may need the already-authorized board reset; an unsupported provider
// function or a readable wrong silicon must never trigger that reset.
bool CirrusAudioFixup::preflightI2CIdentity(const cirrus::platform::AmplifierDriverRegistration* definition) {
    if (!mPlatformAuthorized || bootArgEnabled("-cirrusro") || !definition || !mI2CController)
        return true;
    const auto* board = mPlatformProfile->boardConfiguration;
    if ((board && (!board->amplifiers || !board->count)) ||
        (!board && (!mPlatformProfile->endpoints || !mPlatformProfile->endpointCount))) {
        mAttachment.communication = "RESOURCE_REJECTED";
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return false;
    }
    const uint16_t address = board && board->amplifiers && board->count ? board->amplifiers[0].endpoint.i2cAddress
                             : mPlatformProfile->endpointCount          ? mPlatformProfile->endpoints[0].address
                                                                        : 0;
    if (!address)
        return true;
    if (board && board->amplifiers && board->count) {
        char path[256]{};
        int length = sizeof(path);
        const auto& endpoint = board->amplifiers[0].endpoint;
        if (endpoint.transport != cirrus::core::BusTransportType::I2C || endpoint.tenBitAddress || !mProvider ||
            !mProvider->getPath(path, &length, gIOServicePlane) || !endpoint.controllerPath || strcmp(path, endpoint.controllerPath)) {
            mAttachment.communication = "RESOURCE_REJECTED";
            recordAttachmentFailure("RESOURCE", "I2C_ENDPOINT_UNMATCHED", kIOReturnUnsupported);
            return false;
        }
    }
    cirrus::transport::I2CRegisterBus bus(mI2CController, {address, false});
    cirrus::core::RegisterMap map(bus, *definition->format(cirrus::core::BusTransportType::I2C), false);
    uint32_t id = 0;
    const bool readable = map.read(definition->identityRegister, &id);
    const auto result = map.lastError();
    recordTrace(TRACE_PROBE, 0, false, false, definition->identityRegister, readable ? id : 0, result);
    setProperty("Cirrus_I2C_Preflight_Return", uint64_t(uint32_t(result)), 32);
    setProperty("Cirrus_I2C_Preflight_DeviceID", uint64_t(uint32_t(id)), 32);
    if (!readable && result == kIOReturnUnsupported) {
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "I2C_ADDRESSED_FUNCTION_UNSUPPORTED", result);
        return false;
    }
    if (readable && (id & definition->identityMask) != definition->siliconId) {
        mAttachment.communication = "READY";
        mAttachment.amplifier = "IDENTITY_MISMATCH";
        mPlatformAuthorized = false;
        setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
        recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound);
    }
    return true;
}

void CirrusAudioFixup::recordAttachmentFailure(const char* stage, const char* reason, IOReturn result, uint32_t index,
                                               const char* indexKind) {
    if (!stage)
        stage = "UNKNOWN";
    if (!reason)
        reason = "UNSPECIFIED_FAILURE";
    if (!indexKind)
        indexKind = "UNKNOWN";
    const bool publish = mAttachment.record(stage, reason, result, index, indexKind);
    if (!publish)
        return;
    setProperty("Cirrus_Attach_First_Stage", mAttachment.first.stage);
    setProperty("Cirrus_Attach_First_Reason", mAttachment.first.reason);
    setProperty("Cirrus_Attach_First_Return", uint64_t(uint32_t(mAttachment.first.result)), 32);
    setProperty("Cirrus_Attach_First_Index_Kind", mAttachment.first.indexKind);
    setProperty("Cirrus_Attach_Latest_Index_Kind", indexKind);
    setProperty("Cirrus_Attach_First_Index", uint64_t(uint32_t(mAttachment.first.index)), 32);
    setProperty("Cirrus_Attach_Latest_Stage", mAttachment.latest.stage);
    setProperty("Cirrus_Attach_Latest_Reason", mAttachment.latest.reason);
    setProperty("Cirrus_Attach_Latest_Return", uint64_t(uint32_t(result)), 32);
    setProperty("Cirrus_Attach_Latest_Index", uint64_t(uint32_t(index)), 32);
    setProperty("Cirrus_Attach_Failure_Count", uint64_t(uint32_t(mAttachment.failureCount)), 32);
    char key[80]{};
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Stage", indexKind, index);
    setProperty(key, stage);
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Reason", indexKind, index);
    setProperty(key, reason);
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Return", indexKind, index);
    setProperty(key, uint64_t(uint32_t(result)), 32);
    publishAttachmentStatus();
    CIRRUS_ERR("ATTACH_FAIL stage=%s reason=%s index_kind=%s index=%u return=0x%08X profile=%s board=0x%08X comm=%s amp=%s first=%s", stage,
               reason, indexKind, index, result, mPlatformProfile && mPlatformProfile->name ? mPlatformProfile->name : "unknown",
               mPlatformSubsystemId, mAttachment.communication, mAttachment.amplifier, mAttachment.first.reason);
}
void CirrusAudioFixup::publishAttachmentStatus() {
    setProperty("Cirrus_Communication_Status", mAttachment.communication);
    setProperty("Cirrus_Amplifier_Status", mAttachment.amplifier);
}
