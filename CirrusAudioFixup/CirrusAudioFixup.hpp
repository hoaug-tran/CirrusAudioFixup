//
// CirrusAudioFixup.hpp
// IOService lifecycle, per-amplifier state and diagnostic ownership.
// The workloop serializes playback, sleep and stop. Firmware pointers borrow
// immutable resource data; cached IOKit objects carry explicit retain/release.
// See LICENSE for distribution terms.
//

#pragma once
#include "Transport/I2CRegisterBus.hpp"

#include "Core/AmplifierDescriptors.hpp"
#include "Devices/AmplifierBackends.hpp"
#include "Core/RegisterIO.hpp"
#include "Core/AmplifierLifecycle.hpp"
#include "Core/AmplifierGroup.hpp"
#include "Platform/BoardConfiguration.hpp"
#include "Platform/AmplifierBackendRegistry.hpp"
#include "Platform/AmplifierRuntime.hpp"
#include "Platform/NativeSPIAmplifierRuntime.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include "Transport/VoodooI2CController.hpp"
#include "Devices/CS35L41/CS35L41Device.hpp"
#include "Devices/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Devices/CS35L41/State.hpp"
#include "Devices/CS35L41/Resources/Tuning.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Firmware/WMFW/WMFWUploader.hpp"
#include "Platform/HDA/HDAStreamState.hpp"
#include "Platform/HDA/HDAStreamWatcher.hpp"
#include "Platform/HDA/AudioEventSource.hpp"
#include "Platform/PlatformProfile.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"
#include "Transport/VoodooI2CProvider.hpp"
#include "Devices/CS35L41/CS35L41SPIRegisterIO.hpp"
#include "Transport/ACPISPIResources.hpp"
#include "Transport/NativeSPIHost.hpp"

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

struct RegisterSequence {
    uint32_t reg;
    uint32_t mask;
    uint32_t value;
    uint32_t delayUs;
    bool updateBits;
};

// Platform endpoint binding embeds chip state during the staged ownership
// migration. Register ports and endpoint names borrow the service lifetime.
struct AmplifierState : cirrus::devices::cs35l41::State {
    char endpointLabel[16]{};
    // Identity and channel come from the silicon probe and board endpoint.
    // Do not infer routing or tuning from a display name or slave address.
    const char* name{nullptr};
    uint8_t address{0};
    // Borrowed native register port; null selects the legacy addressed I2C ABI.
    cirrus::core::RegisterIO* registerIO{nullptr};
    cirrus::core::BusEndpoint endpoint{};
    uint8_t endpointIndex{0};
    cirrus::core::AmplifierModel model{cirrus::core::AmplifierModel::Unknown};
    cirrus::core::AudioChannel channel{cirrus::core::AudioChannel::Left};
};

struct FirmwareImage;

struct FirmwareResource {
    uint32_t subsystemVendor;
    uint32_t subsystemDevice;
    uint32_t spkid;
    const char* fwName;
    const char* binName;
    const uint8_t* wmfw;
    size_t wmfwSize;
    const uint8_t* bin;
    size_t binSize;
    const uint8_t* binRight;
    size_t binRightSize;
    bool isDummy;
    // Optional companions belong to the same board/speaker entry as its BIN.
    // Absence on one channel uses the Linux default, never the other channel.
    const uint8_t* tuningLeft{nullptr};
    size_t tuningLeftSize{0};
    const uint8_t* tuningRight{nullptr};
    size_t tuningRightSize{0};
    const char* tuningLeftName{nullptr};
    const char* tuningRightName{nullptr};
};

class CirrusAudioFixup : public IOService {
    friend class ServiceRegisterIO;

    OSDeclareDefaultStructors(CirrusAudioFixup)

public:
    bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService* device) override;

private:
    void bringUpAmplifiers();
    IOReturn initializeCS35L41Hardware(AmplifierState& amp);
    IOReturn loadCS35L41Firmware(AmplifierState& amp);
    IOReturn verifyCS35L41Ready(AmplifierState& amp);
    IOReturn shutdownCS35L41(AmplifierState& amp);
    bool stopCS35L41At(AmplifierState& amp,const char* phase);
    // Transitional adapter: no ownership transfer. Clear only after runtime
    // unbind has completed shutdown and destroyed every borrowing backend.
    struct AmplifierBackendOwner final : cirrus::devices::cs35l41::Platform {
        CirrusAudioFixup* owner{nullptr};
        AmplifierState* amp{nullptr};
        void bind(CirrusAudioFixup* service, AmplifierState* endpoint) { owner = service; amp = endpoint; }
        void clear() { owner = nullptr; amp = nullptr; }
        cirrus::devices::cs35l41::State* state() const override { return owner && amp ? amp : nullptr; }
        cirrus::core::BusEndpoint endpoint() const override { return amp->endpoint; }
        cirrus::core::AudioChannel channel() const override { return amp->channel; }
        cirrus::core::AmplifierModel model() const override { return amp->model; }
        bool authorized() const override { return owner->mPlatformAuthorized; }
        IOReturn initialize() override { return owner->initializeCS35L41Hardware(*amp); }
        IOReturn loadFirmware() override { return owner->loadCS35L41Firmware(*amp); }
        IOReturn verifyReady() override { return owner->verifyCS35L41Ready(*amp); }
        IOReturn commitPlayback() override { return owner->commitCS35L41Playback(*amp); }
        bool stopPlayback() override { return owner->stopPlayback(*amp); }
        IOReturn shutdown() override { return owner->shutdownCS35L41(*amp); }
        void captureDiagnostics() override { owner->captureCS35L41FailureSnapshot(*amp, amp->diagnostic.latestFailure); }
        bool configureInterrupt(bool enabled) override { return owner->configureCS35L41Interrupt(*amp, enabled); }
        bool checkProtection() override { return owner->checkProtectionStatus(*amp); }
        void monitor(const cirrus::core::PlaybackContext& context) override { owner->monitorCS35L41(*amp, context); }
        bool validatePlayback(const cirrus::core::PlaybackContext& context) override { return owner->validateCS35L41Playback(*amp, context); }
        void cancelPlayback() override { owner->stopRuntimePlayback(*amp); }
        void recordIdentityFailure(uint32_t actual, IOReturn result) override {
            owner->recordDiagnosticFailure(*amp, cirrus::diagnostics::DIAG_DEVICE_ID,
                cirrus::devices::cs35l41::registers::kRegDeviceId,
                cirrus::devices::cs35l41::registers::kValDeviceId, actual, result);
        }
    };
    IOReturn commitCS35L41Playback(AmplifierState& amp);
    bool configureCS35L41Interrupt(AmplifierState& amp,bool enabled);
    bool validateCS35L41Playback(AmplifierState& amp,const cirrus::core::PlaybackContext&);
    void monitorCS35L41(AmplifierState& amp,const cirrus::core::PlaybackContext&);
    char mRuntimeControllerPath[256]{};
    bool bindAmplifierRuntime();
    void teardownAmplifierRuntime();
    bool registerAmplifierBackends();

    void runBackgroundMonitor();
    bool retryFirmwareIdentity();
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
    bool setupNativeSPI(IOService* provider);
    size_t detectNativeSPIAmplifiers();
    size_t detectConfiguredI2CAmplifiers();
    void teardownNativeSPI();
    void suspendNativeTransport() {
        mNativeSPI.suspend();
        if (mNativeSPI.bound()) setProperty("Cirrus_SPI_Status", "SUSPENDED");
    }
    bool resumeNativeTransport() {
        IOReturn ret = mNativeSPI.resume();
        if (mNativeSPI.bound()) {
            setProperty("Cirrus_SPI_Wake_Return", uint64_t(ret), 32);
            setProperty("Cirrus_SPI_Status",ret==kIOReturnSuccess?(mPlatformAuthorized?"BOUND_AUTHORIZED":"BOUND_READ_ONLY"):"WAKE_FAILED");
            const auto& failure=mNativeSPI.failure();
            setProperty("Cirrus_SPI_Failure_Reason",failure.reason);
            setProperty("Cirrus_SPI_Failure_Index",uint64_t(failure.index),32);
            setProperty("Cirrus_SPI_Failure_Source",failure.source);
            setProperty("Cirrus_SPI_Failure_Detail",uint64_t(failure.detail),32);
            if(ret!=kIOReturnSuccess)CIRRUS_ERR("SPI wake blocked: reason=%s controller=%lu detail=0x%08X ret=0x%08X; amplifiers remain suspended",failure.reason,failure.index,failure.detail,ret);
        }
        return ret == kIOReturnSuccess;
    }
    bool nativeBusReady() const { return mProvider && mPowerAvailable && !mStopping; }
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
    static constexpr size_t kMaxAmplifiers = cirrus::platform::AmplifierRuntime::kCapacity;
    size_t mAmplifierCount{0};
    AmplifierState mAmplifiers[kMaxAmplifiers]{};
    const cirrus::platform::PlatformProfile* mPlatformProfile{nullptr};
    size_t expectedAmplifierCount() const {
        return mPlatformProfile?(mPlatformProfile->boardConfiguration?mPlatformProfile->boardConfiguration->count:mPlatformProfile->endpointCount):0;
    }
    bool mPlatformAuthorized{false};
    uint32_t mPlatformSubsystemId{0};
    cirrus::platform::AmplifierBackendRegistry mBackendRegistry;
    cirrus::platform::AmplifierRuntime mAmplifierRuntime{false};
    cirrus::core::RegisterIO* mRuntimePorts[kMaxAmplifiers]{};
    cirrus::transport::VoodooI2CController* mI2CController{nullptr};
    cirrus::transport::I2CRegisterBus* mI2CBuses[kMaxAmplifiers]{};
    cirrus::core::RegisterMap* mI2CMaps[kMaxAmplifiers]{};
    AmplifierBackendOwner mBackendOwners[kMaxAmplifiers]{};
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
    void captureCS35L41FailureSnapshot(AmplifierState& amp,cirrus::diagnostics::DiagnosticFailure failure);
    void publishDriverVerdict();
    static const char* stageName(cirrus::diagnostics::DriverStage stage);
    static const char* failureName(cirrus::diagnostics::DiagnosticFailure failure);

    bool bootArgEnabled(const char* name);
    bool bootArgStrEquals(const char* name, const char* expectedVal);
    void logProviderInfo(IOService* provider);
    void dumpProviderProperties(IOService* provider);
    void resolvePlatformProfile();
    bool setupProbeTimer();
    void scheduleReadOnlyProbe(uint32_t delayMs);
    void runReadOnlyProbe();
    void probeAmplifier(AmplifierState& amp);
    size_t detectAmplifiers();

    bool mProbingAmplifiers{false};

    bool transferToAddress(uint8_t address, uint8_t* writeBuffer, uint16_t writeLength, uint8_t* readBuffer, uint16_t readLength);

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

    bool sendMailboxCommand(AmplifierState& amp, uint32_t command, uint32_t expectedStatus);

    void logASPSnapshot(AmplifierState& amp);
    void logDSPSnapshot(AmplifierState& amp);
    void logDSPBootReport(AmplifierState& amp);
    void logPowerSnapshot(AmplifierState& amp);
    void snapshotPlayback(AmplifierState& amp);
    void snapshotDiagnostics(AmplifierState& amp, const char* stage);

    bool initializeCS35L41Device(AmplifierState& amp);

    bool unlockTestKey(AmplifierState& amp);
    bool lockTestKey(AmplifierState& amp);

    bool applyErrataPatch(AmplifierState& amp);

    bool unpackOTP(AmplifierState& amp);

    bool initializeHardwareErrata(AmplifierState& amp);
    void dumpAllRegisters(AmplifierState& amp);
    bool configureHardware(AmplifierState& amp);

    bool synchronizeHdaStream();
    bool synchronizeHdaCodec() { return synchronizeHdaStream(); }
    bool performPlatformHardwareReset();
    static bool supportedHdaFormat(uint16_t format);

    void discoverFirmware(AmplifierState& amp);

    bool bringUpDSP(AmplifierState& amp);
    bool bootDsp(AmplifierState& amp) { return bringUpDSP(amp); }

    void startBringUp() { bringUpAmplifiers(); }
    bool resetAndInitCodec(AmplifierState& amp) { return initializeCS35L41Device(amp); }
    bool loadOtpCalibration(AmplifierState& amp) { return unpackOTP(amp); }

    bool stopPlayback(AmplifierState& amp);
    bool stopRuntimePlayback(AmplifierState& amp);
    bool checkProtectionStatus(AmplifierState& amp);
    bool verifyDSPAlive(AmplifierState& amp);

    void uploadFirmware(AmplifierState& amp, const char* phaseArg);
    bool parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage);
    bool stopDSP(AmplifierState& amp);

    bool applyCalibration(AmplifierState& amp, const FirmwareImage* image);
    void initializeFirmware(AmplifierState& amp, const char* phaseArg);
    void dumpASPRegisters(AmplifierState& amp);

    bool powerUpAmplifier(AmplifierState& amp);
    bool verifyIdleConfiguration(AmplifierState& amp);

    IOService* audioController();
    IOService* getAudioController();

    void testRegisterConsistency(AmplifierState& amp);
    void runTimeBasedFSMCheck(AmplifierState& amp);

    uint32_t calculateRegistersCRC32(AmplifierState& amp);

    void snapshotRegisters(AmplifierState& amp, uint32_t* snapshot);
    void compareRegisterSnapshots(AmplifierState& amp, const uint32_t* oldSnapshot, const uint32_t* newSnapshot);

    bool applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count);

    bool applyPLL(AmplifierState& amp);
    bool configurePll(AmplifierState& amp) { return applyPLL(amp); }

    bool applyASP(AmplifierState& amp);
    bool configureAsp(AmplifierState& amp) { return applyASP(amp); }

    bool applyGPIO(AmplifierState& amp);
    bool configureGpio(AmplifierState& amp) { return applyGPIO(amp); }

    static void probeTimerFired(OSObject* owner, IOTimerEventSource* sender);
};
