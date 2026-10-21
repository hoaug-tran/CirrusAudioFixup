// Independent CS35L41 implementation and owned live state.
// Host/register/board/asset views outlive this driver through shutdown.
#pragma once
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"
#include "Devices/Amplifiers/CS35L41/InstanceState.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/ResourceSchema.hpp"
#include "Runtime/AmplifierDriverRegistry.hpp"
#include "Runtime/AmplifierHost.hpp"
#include "Platform/HDA/HDAStreamState.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
struct FirmwareImage;
namespace cirrus::devices::amplifiers::cs35l41 {
class CS35L41Driver final : public core::AmplifierLifecycleHooks {
public:
    explicit CS35L41Driver(const platform::AmplifierDriverContext&);
    core::BringUpDisposition disposition() const override { return mAmplifier.bringUpDisposition; }
    IOReturn identify() override;
    IOReturn authorizeBoard() override;
    IOReturn initialize() override;
    IOReturn loadFirmware() override;
    IOReturn verifyReady() override;
    IOReturn startPlayback() override;
    IOReturn stopPlayback() override;
    IOReturn safeShutdown() override;
    void captureDiagnostics() override;
    IOReturn configureInterrupt(bool) override;
    IOReturn handleInterrupt() override;
    bool managesPlaybackTransitions() const override { return true; }
    IOReturn runtimeTick(const core::PlaybackContext&) override;
    bool playbackNeedsPreparation() const override;
    bool playbackStartCancelled() const override { return mAmplifier.playbackStartCancelled; }
    bool playbackPrepared() const override;
    bool playbackPending() const override { return mAmplifier.playbackTransition.pending() || mAmplifier.shutdownTransition.pending(); }
    bool playbackFaulted() const override;
    bool validatePlayback(const core::PlaybackContext&) override;
    IOReturn cancelPlayback() override;
    core::ObservedPlayback observedPlayback() const override {
        return mAmplifier.playbackActive ? core::ObservedPlayback::Active : core::ObservedPlayback::Idle;
    }
    core::AmplifierReport report() const override;
    IOReturn prepareBringUpRetry(uint64_t) override;
    void inspect();

private:
    friend class DriverRegisterIO;
    using AmplifierState = InstanceState;
    struct RegisterSequence {
        uint32_t reg, mask, value, delayUs;
        bool updateBits;
    };
    const platform::AmplifierDriverContext& mContext;
    platform::AmplifierHost& mHost;
    InstanceState mAmplifier{};
    platform::hda::HDAStreamState mHdaState{};
    struct RouteGuard {
        platform::AmplifierHost& host;
        bool unchanged(uint32_t generation) { return host.routeUnchanged(generation); }
    } mRouteGuard;
    RouteGuard* mAudioEvents{nullptr};
    IOReturn mLastTransferReturn{kIOReturnSuccess};
    bool mCapturingFailureSnapshot{false};
    void refreshRoute();
    bool synchronizeHdaStream();
    bool stopAt(const char*);
    IOReturn initializeHardware();
    IOReturn loadFirmwareFlow();
    IOReturn verifyReadyFlow();
    IOReturn shutdownFlow();
    bool bootArgEnabled(const char* name) { return mHost.bootArgEnabled(name); }
    bool bootArgStrEquals(const char* name, const char* value) { return mHost.bootArgStrEquals(name, value); }
    bool stopAfterDebugStage(const char* phase) { return mHost.stopAfterDebugStage(phase); }
    bool parseBootArgument(const char* name, void* value, size_t size) { return mHost.parseBootArgument(name, value, size); }
    IOService* getAudioController() { return mHost.audioController(); }
    static OSDictionary* serviceMatching(const char* name) { return IOService::serviceMatching(name); }
    static OSIterator* getMatchingServices(OSDictionary* match) { return IOService::getMatchingServices(match); }
    void setProperty(const char* name, const char* value) { mHost.publish(name, value); }
    void setProperty(const char* name, uint64_t value, unsigned bits) { mHost.publish(name, value, bits); }
    void setProperty(const char* name, OSObject* value) { mHost.publish(name, value); }
    void removeProperty(const char* name) { mHost.remove(name); }
    void publishDriverVerdict() { mHost.publishVerdict(); }
    void setDiagnosticStage(AmplifierState&, diagnostics::DriverStage);
    void markDiagnosticSuccess(AmplifierState&, diagnostics::DriverStage);
    void recordDiagnosticFailure(AmplifierState&, diagnostics::DiagnosticFailure, uint32_t reg = 0, uint32_t expected = 0,
                                 uint32_t actual = 0, IOReturn result = kIOReturnSuccess, bool captureSnapshot = true);
    bool readRegister(AmplifierState&, uint32_t, uint32_t*, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool writeRegister(AmplifierState&, uint32_t, uint32_t, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool bulkRead(AmplifierState&, uint32_t, uint8_t*, size_t, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool bulkWrite(AmplifierState&, uint32_t, const uint8_t*, size_t, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool updateRegisterBits(AmplifierState&, uint32_t, uint32_t, uint32_t, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool pollRegisterBit(AmplifierState&, uint32_t, uint32_t, uint32_t, uint32_t, diagnostics::TraceSource = diagnostics::TRACE_OTHER);
    bool configureCS35L41Interrupt(AmplifierState& amp, bool enabled);
    void initializeFirmware(AmplifierState& amp, const char* phaseArg);
    bool applyCalibration(AmplifierState& amp, const FirmwareImage* image);
    bool checkProtectionStatus(AmplifierState& amp);
    bool stopPlayback(AmplifierState& amp);
    bool stopRuntimePlayback(AmplifierState& amp);
    bool validateCS35L41Playback(AmplifierState& amp, const cirrus::core::PlaybackContext& context);
    IOReturn commitCS35L41Playback(AmplifierState& amp);
    void monitorCS35L41(AmplifierState& amp, const cirrus::core::PlaybackContext& context);
    void probeAmplifier(AmplifierState& amp);
    void captureCS35L41FailureSnapshot(AmplifierState& amp, diagnostics::DiagnosticFailure failure);
    bool sendMailboxCommand(AmplifierState& amp, UInt32 command, UInt32 expectedStatus);
    bool initializeCS35L41Device(AmplifierState& amp);
    uint32_t calculateRegistersCRC32(AmplifierState& amp);
    void dumpAllRegisters(AmplifierState& amp);
    void runTimeBasedFSMCheck(AmplifierState& amp);
    bool unlockTestKey(AmplifierState& amp);
    bool lockTestKey(AmplifierState& amp);
    bool initializeHardwareErrata(AmplifierState& amp);
    bool applyErrataPatch(AmplifierState& amp);
    bool unpackOTP(AmplifierState& amp);
    void snapshotRegisters(AmplifierState& amp, UInt32* snapshot);
    void compareRegisterSnapshots(AmplifierState& amp, const UInt32* oldSnapshot, const UInt32* newSnapshot);
    bool applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count);
    bool applyPLL(AmplifierState& amp);
    bool applyASP(AmplifierState& amp);
    bool applyGPIO(AmplifierState& amp);
    bool stopDSP(AmplifierState& amp);
    void discoverFirmware(AmplifierState& amp);
    bool bringUpDSP(AmplifierState& amp);
    bool verifyDSPAlive(AmplifierState& amp);
    bool parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage);
    void uploadFirmware(AmplifierState& amp, const char* phaseArg);
    bool configureHardware(AmplifierState& amp);
    void logASPSnapshot(AmplifierState& amp);
    void logDSPSnapshot(AmplifierState& amp);
    void logDSPBootReport(AmplifierState& amp);
    void snapshotPlayback(AmplifierState& amp);
    void snapshotDiagnostics(AmplifierState& amp, const char* stage);
    void logPowerSnapshot(AmplifierState& amp);
    bool powerUpAmplifier(AmplifierState& amp);
    bool verifyIdleConfiguration(AmplifierState& amp);
};
}
