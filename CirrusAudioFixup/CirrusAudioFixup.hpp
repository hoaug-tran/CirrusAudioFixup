//
// CirrusAudioFixup.hpp
// IOService lifecycle, endpoint resources and generic diagnostic publication.
// The workloop serializes playback, sleep and stop. Driver-owned chip state
// is destroyed before borrowed ports/hosts; IOKit objects use retain/release.
// See LICENSE for distribution terms.
//

#pragma once
#include "Transport/I2CRegisterBus.hpp"

#include "Core/AmplifierGroup.hpp"
#include "Core/AmplifierLifecycle.hpp"
#include "Core/RegisterIO.hpp"
#include "Devices/AmplifierDrivers.hpp"
#include "Diagnostics/AttachmentState.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include "Platform/AmplifierHost.hpp"
#include "Platform/AmplifierManager.hpp"
#include "Platform/BoardConfiguration.hpp"
#include "Platform/HDA/AudioEventSource.hpp"
#include "Platform/HDA/HDAStreamState.hpp"
#include "Platform/HDA/HDAStreamWatcher.hpp"
#include "Platform/PlatformProfile.hpp"
#include "Platform/SPIAmplifierManager.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"
#include "Transport/ACPISPIResources.hpp"
#include "Transport/NativeSPIHost.hpp"
#include "Transport/VoodooI2CController.hpp"
#include "Transport/VoodooI2CProvider.hpp"

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSCollectionIterator.h>

#include <os/log.h>
#include <pexpert/pexpert.h>

// Endpoint metadata and host diagnostics only. Silicon state belongs to driver.
struct AmplifierState {
    char endpointLabel[16]{};
    const char* name{nullptr};
    uint8_t address{0};
    cirrus::core::RegisterIO* registerIO{nullptr};
    cirrus::core::BusEndpoint endpoint{};
    uint8_t endpointIndex{0};
    cirrus::core::AmplifierModel model{cirrus::core::AmplifierModel::Unknown};
    cirrus::core::AudioChannel channel{cirrus::core::AudioChannel::Left};
    bool present{false}, initialized{false}, playbackFaulted{false};
    uint32_t deviceId{0}, revisionId{0};
    cirrus::diagnostics::DiagnosticState diagnostic{};
};

class CirrusAudioFixup : public IOService {
    friend class ServiceRegisterIO;

OSDeclareDefaultStructors(CirrusAudioFixup)

    public : bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService* device) override;

private:
    void bringUpAmplifiers();
    // Borrowed serialized host facilities; cleared after driver destruction.
    struct AmplifierHostAdapter final : cirrus::platform::AmplifierHost {
        CirrusAudioFixup* owner{nullptr};
        AmplifierState* amp{nullptr};
        void bind(CirrusAudioFixup* service, AmplifierState* endpoint) {
            owner = service;
            amp = endpoint;
        }
        void clear() {
            owner = nullptr;
            amp = nullptr;
        }
        bool authorized() const override { return owner && owner->mPlatformAuthorized; }
        const char* name() const override { return amp->name; }
        bool supportsResources() const override { return owner && amp; }
        uint8_t endpointIndex() const override { return amp->endpointIndex; }
        const cirrus::diagnostics::DiagnosticState& diagnostics() const override { return amp->diagnostic; }
        void setDiagnosticStage(cirrus::diagnostics::DriverStage stage) override { owner->setDiagnosticStage(*amp, stage); }
        void markDiagnosticSuccess(cirrus::diagnostics::DriverStage stage) override { owner->markDiagnosticSuccess(*amp, stage); }
        void recordDiagnosticFailure(cirrus::diagnostics::DiagnosticFailure failure, uint32_t reg, uint32_t expected, uint32_t actual,
                                     IOReturn result, bool capture = true) override {
            owner->recordDiagnosticFailure(*amp, failure, reg, expected, actual, result, capture);
        }
        bool bootArgEnabled(const char* name) const override { return owner->bootArgEnabled(name); }
        bool bootArgStrEquals(const char* name, const char* value) const override { return owner->bootArgStrEquals(name, value); }
        bool parseBootArgument(const char* name, void* value, size_t size) const override {
            return PE_parse_boot_argn(name, value, int(size));
        }
        bool stopAfterDebugStage(const char* phase) override { return owner->stopAfterDebugStage(phase); }
        bool debugHalted() const override { return owner->mDebugPhaseHalted; }
        void publish(const char* key, const char* value) override { owner->setProperty(key, value); }
        void publish(const char* key, uint64_t value, unsigned bits) override { owner->setProperty(key, value, bits); }
        void publish(const char* key, OSObject* value) override { owner->setProperty(key, value); }
        void remove(const char* key) override { owner->removeProperty(key); }
        void publishVerdict() override { owner->publishDriverVerdict(); }
        IOService* audioController() override { return owner->audioController(); }
        cirrus::platform::hda::HDAStreamState streamObservation() const override { return owner->mHdaState; }
        bool hasRouteEvents() const override { return owner->mAudioEvents != nullptr; }
        bool routeUnchanged(uint32_t generation) const override {
            return owner->mAudioEvents && owner->mAudioEvents->unchanged(generation);
        }
        bool synchronizeRoute() override { return owner->synchronizeHdaStream(); }
        void beginDiagnosticCapture() override { owner->mCapturingFailureSnapshot = true; }
        void endDiagnosticCapture() override { owner->mCapturingFailureSnapshot = false; }
    };
    char mRuntimeControllerPath[256]{};
    bool bindAmplifierDrivers();
    void releaseAmplifierDrivers();
    bool registerAmplifierDrivers();
    cirrus::transport::I2CController* createI2CController();

    void runBackgroundMonitor();
    bool retryBringUpResources();
    bool setupAudioEvents();
    void teardownAudioEvents();
    static void audioEventReceived(OSObject* owner, const cirrus::platform::hda::AudioEventState& state);
    cirrus::platform::hda::AudioEventSource* mAudioEvents{nullptr};
    cirrus::platform::hda::AudioEventState mAudioState{};
    // Preparation is retried only within a bounded transition window. A new
    // output/engine event starts a new window; headphone idle never arms it.
    uint32_t mAudioPrepareRetries{0};

    // The provider is borrowed for the attached service lifetime. The workloop
    // and its event sources are owned and removed before stop clears it.
    IOService* mProvider{nullptr};
    cirrus::transport::NativeSPIHost mNativeSPI;
    bool setupCommunication(IOService* provider);
    bool retryPlatformIdentity();
    bool mPlatformIdentityPending{false};
    uint64_t mPlatformIdentityDeadlineMs{0};
    bool preflightI2CIdentity(const cirrus::platform::AmplifierDriverRegistration*);
    size_t detectNativeSPIAmplifiers();
    size_t detectConfiguredI2CAmplifiers();
    void teardownCommunication();
    void suspendCommunication() {
        mNativeSPI.suspend();
        if (mNativeSPI.bound())
            mAttachment.communication = "SUSPENDED";
        else if (mI2CController)
            mAttachment.communication = "PROVIDER_PM_OWNED";
        publishAttachmentStatus();
        if (mNativeSPI.bound())
            setProperty("Cirrus_SPI_Status", "SUSPENDED");
    }
    bool resumeCommunication() {
        IOReturn ret = mNativeSPI.resume();
        if (mNativeSPI.bound()) {
            setProperty("Cirrus_SPI_Wake_Return", uint64_t(uint32_t(ret)), 32);
            setProperty("Cirrus_SPI_Status",
                        ret == kIOReturnSuccess ? (mPlatformAuthorized ? "BOUND_AUTHORIZED" : "BOUND_READ_ONLY") : "WAKE_FAILED");
            const auto& failure = mNativeSPI.failure();
            setProperty("Cirrus_SPI_Failure_Reason", failure.reason);
            setProperty("Cirrus_SPI_Failure_Index", uint64_t(uint32_t(failure.index)), 32);
            setProperty("Cirrus_SPI_Failure_Source", failure.source);
            setProperty("Cirrus_SPI_Failure_Detail", uint64_t(uint32_t(failure.detail)), 32);
            mAttachment.communication = ret == kIOReturnSuccess ? (mPlatformAuthorized ? "READY" : "READ_ONLY") : "FAILED";
            publishAttachmentStatus();
            if (ret != kIOReturnSuccess)
                recordAttachmentFailure("CONTROLLER_PM", failure.reason, ret, uint32_t(failure.index), "CONTROLLER_SLOT");
            if (ret != kIOReturnSuccess)
                CIRRUS_ERR("SPI wake blocked: reason=%s controller=%lu detail=0x%08X ret=0x%08X; amplifiers remain suspended",
                           failure.reason, failure.index, failure.detail, ret);
        }
        return ret == kIOReturnSuccess;
    }
    bool communicationReady() const { return mProvider && mPowerAvailable && !mStopping; }
    IOWorkLoop* mWorkLoop{nullptr};
    IOTimerEventSource* mProbeTimer{nullptr};
    IOCommandGate* mCommandGate{nullptr};
    bool mPMInitialized{false};
    IOPMPowerState mPowerStates[2]{};
    bool mPowerAvailable{true};
    bool mStopping{false};
    bool mNeedsReinitialization{false};
    bool mDebugPhaseHalted{false};
    bool stopAfterDebugStage(const char* stage);

    IOInterruptEventSource* mAmpInterrupt{nullptr};
    bool setupAmplifierInterrupt();
    bool configureAmplifierInterrupt();
    void teardownAmplifierInterrupt();
    static void amplifierInterruptReceived(OSObject*, IOInterruptEventSource*, int);

    bool setupPowerManagement(IOService* provider);
    void handlePowerChange(bool powered);
    static IOReturn lifecycleAction(OSObject* owner, void* operation, void*, void*, void*);

    cirrus::platform::hda::HDAStreamState mHdaState;
    // Owned references, valid only while the lifecycle allows HDA access.
    IOPCIDevice* mHdaPci{nullptr};
    IOMemoryMap* mHdaMap{nullptr};
    // Points to a status string literal; no allocation or release is needed.
    const char* mHdaStatus{nullptr};
    void clearHdaCache();

    IOReturn mLastTransferReturn{kIOReturnSuccess};
    bool mCapturingFailureSnapshot{false};

    // Storage capacity is not supported topology. Current profiles permit
    // only the two channels with matching routing and speaker tuning.
    static constexpr size_t kMaxAmplifiers = cirrus::platform::AmplifierManager::kCapacity;
    size_t mAmplifierCount{0};
    AmplifierState mAmplifiers[kMaxAmplifiers]{};
    cirrus::diagnostics::RuntimeDiagnosticSnapshot mRuntimePublished[kMaxAmplifiers]{};
    uint32_t mPublishedRuntimeStage{UINT32_MAX}, mPublishedRuntimeCount{UINT32_MAX};
    const cirrus::platform::PlatformProfile* mPlatformProfile{nullptr};
    size_t expectedAmplifierCount() const {
        return mPlatformProfile
                   ? (mPlatformProfile->boardConfiguration ? mPlatformProfile->boardConfiguration->count : mPlatformProfile->endpointCount)
                   : 0;
    }
    bool mPlatformAuthorized{false};
    uint32_t mPlatformSubsystemId{0};
    cirrus::platform::AmplifierDriverRegistry mDriverRegistry;
    cirrus::platform::AmplifierManager mAmplifierManager{false};
    cirrus::core::RegisterIO* mRuntimePorts[kMaxAmplifiers]{};
    cirrus::transport::I2CController* mI2CController{nullptr};
    cirrus::transport::I2CRegisterBus* mI2CBuses[kMaxAmplifiers]{};
    cirrus::core::RegisterMap* mI2CMaps[kMaxAmplifiers]{};
    AmplifierHostAdapter mHostAdapters[kMaxAmplifiers]{};
    cirrus::platform::AmplifierBinding mRuntimeBindings[kMaxAmplifiers]{};
    cirrus::firmware::FirmwareBundle mRuntimeFirmware[kMaxAmplifiers]{};
    cirrus::platform::BoardConfiguration mRuntimeBoard{};

    static const size_t kTraceBufferSize = 1024;
    cirrus::diagnostics::TraceEntry mTraceBuffer[kTraceBufferSize];
    uint32_t mTraceHead{0};
    uint32_t mTraceTail{0};
    cirrus::diagnostics::TraceStats mTraceStats{};
    // Protects the flight recorder, not the hardware lifecycle.
    IOLock* mTraceLock{nullptr};

    void initTraceBuffer();
    void recordTrace(cirrus::diagnostics::TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen,
                     IOReturn ret);
    void dumpTraceBuffer(const char* propertyName = "Cirrus_Trace_Dump", const char* mirrorPropertyName = nullptr);
    void publishStatistics();
    void setDiagnosticStage(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void markDiagnosticSuccess(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void recordDiagnosticFailure(AmplifierState& amp, cirrus::diagnostics::DiagnosticFailure failure, uint32_t reg = 0,
                                 uint32_t expected = 0, uint32_t actual = 0, IOReturn ioReturn = kIOReturnSuccess,
                                 bool captureSnapshot = true);
    void captureFailureSnapshot(AmplifierState& amp, cirrus::diagnostics::DiagnosticFailure failure);
    void publishDriverVerdict();
    bool publishRuntimeStatus();
    static const char* stageName(cirrus::diagnostics::DriverStage stage);
    static const char* failureName(cirrus::diagnostics::DiagnosticFailure failure);

    bool bootArgEnabled(const char* name);
    bool bootArgStrEquals(const char* name, const char* expectedVal);
    cirrus::diagnostics::AttachmentState mAttachment{};
    void recordAttachmentFailure(const char* stage, const char* reason, IOReturn result, uint32_t index = 0,
                                 const char* indexKind = "ENDPOINT");
    void publishAttachmentStatus();
    void logProviderInfo(IOService* provider);
    void dumpProviderProperties(IOService* provider);
    void resolvePlatformProfile();
    bool setupProbeTimer();
    void scheduleReadOnlyProbe(uint32_t delayMs);
    void runReadOnlyProbe();
    void probeAmplifier(AmplifierState&);
    size_t detectAmplifiers();

    bool mProbingAmplifiers{false};

public:
    bool bulkWrite(AmplifierState& amp, uint32_t reg, const uint8_t* data, size_t length,
                   cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool bulkRead(AmplifierState& amp, uint32_t reg, uint8_t* data, size_t length,
                  cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

private:
    bool readRegister(AmplifierState& amp, uint32_t reg, uint32_t* value,
                      cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool writeRegister(AmplifierState& amp, uint32_t reg, uint32_t value,
                       cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool updateRegisterBits(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t value,
                            cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool pollRegisterBit(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs,
                         cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

    bool synchronizeHdaStream();
    bool synchronizeHdaCodec() { return synchronizeHdaStream(); }
    bool performPlatformHardwareReset();
    static bool supportedHdaFormat(uint16_t format);

    void startBringUp() { bringUpAmplifiers(); }

    IOService* audioController();
    IOService* getAudioController();

    static void probeTimerFired(OSObject* owner, IOTimerEventSource* sender);
};
