// Independent CS35L56 implementation. Host, board and register views outlive it.
#pragma once
#include "Devices/Amplifiers/CS35L56/InstanceState.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/ResourceSchema.hpp"
#include "Runtime/AmplifierDriverRegistry.hpp"
#include "Runtime/AmplifierHost.hpp"
struct FirmwareImage;

namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56Driver final : public core::AmplifierLifecycleHooks {
public:
    explicit CS35L56Driver(const platform::AmplifierDriverContext& context) : mContext(context), mHost(*context.host) {}
    void inspect();
    IOReturn identify() override;
    IOReturn authorizeBoard() override;
    IOReturn initialize() override;
    IOReturn loadFirmware() override;
    IOReturn verifyReady() override;
    IOReturn startPlayback() override;
    IOReturn stopPlayback() override;
    IOReturn cancelPlayback() override {
        mAmplifier.routePrepared = false;
        return mAmplifier.playbackActive ? stopPlayback() : kIOReturnSuccess;
    }
    IOReturn safeShutdown() override;
    IOReturn runtimeTick(const core::PlaybackContext&) override;
    bool validatePlayback(const core::PlaybackContext&) override;
    bool managesPlaybackTransitions() const override { return true; }
    bool playbackPrepared() const override { return mAmplifier.routePrepared && !mAmplifier.faulted; }
    bool playbackFaulted() const override { return mAmplifier.faulted; }
    core::ObservedPlayback observedPlayback() const override {
        return mAmplifier.playbackActive ? core::ObservedPlayback::Active : core::ObservedPlayback::Idle;
    }
    core::BringUpDisposition disposition() const override { return mAmplifier.bringUpDisposition; }
    core::AmplifierReport report() const override;

private:
    const platform::AmplifierDriverContext& mContext;
    platform::AmplifierHost& mHost;
    InstanceState mAmplifier{};
    const FirmwareResource* mResource{nullptr};
    void probeAmplifier();
    bool writable() const;
    bool stopAt(const char*);
    IOReturn fail(IOReturn, diagnostics::DiagnosticFailure, uint32_t reg = 0, uint32_t expected = 0, uint32_t actual = 0);
    IOReturn transferFailure(diagnostics::DiagnosticFailure, uint32_t reg = 0);
    void publishState(const char*);
    IOReturn checkProtectionStatus();
    bool applyCalibration(FirmwareImage&);
    static void delayUs(uint32_t);
};
}
