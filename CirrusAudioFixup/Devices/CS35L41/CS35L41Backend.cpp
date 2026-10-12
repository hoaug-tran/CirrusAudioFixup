// CS35L41 lifecycle backend. The IOService lends serialized platform resources
// and register ports; concrete silicon sequences remain CS35L41-specific.
#include "Devices/CS35L41/CS35L41Backend.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Support/Logging.hpp"
using namespace cirrus::diagnostics;

namespace cirrus::devices::cs35l41 {

class Backend final : public cirrus::core::AmplifierLifecycleHooks {
    Platform& mPlatform;
    State& mAmplifier;
    const cirrus::platform::AmplifierBackendContext& mContext;

public:
    explicit Backend(const cirrus::platform::AmplifierBackendContext& c, Platform& platform)
        : mPlatform(platform), mAmplifier(*platform.state()), mContext(c) {}
    cirrus::core::BringUpDisposition disposition() const override { return mAmplifier.bringUpDisposition; }
    IOReturn identify() override {
        uint32_t id = 0;
        if (!mContext.io.read(registers::kRegDeviceId, &id)) {
            const auto error = mContext.io.lastError();
            const IOReturn result = error == kIOReturnSuccess || error == -1 ? kIOReturnNotReady : error;
            mAmplifier.playbackFaulted = true;
            mPlatform.recordDiagnosticFailure(DIAG_DEVICE_ID, registers::kRegDeviceId, registers::kValDeviceId, id, result);
            return result;
        }
        if (id != registers::kValDeviceId) {
            mAmplifier.playbackFaulted = true;
            mPlatform.recordDiagnosticFailure(DIAG_DEVICE_ID, registers::kRegDeviceId, registers::kValDeviceId, id, kIOReturnNotFound);
            return kIOReturnNotFound;
        }
        return kIOReturnSuccess;
    }
    IOReturn authorizeBoard() override {
        return mPlatform.authorized() && mContext.board.permits(mContext.actualBoard) &&
                       (mPlatform.channel() == cirrus::core::AudioChannel::Left || mPlatform.channel() == cirrus::core::AudioChannel::Right)
                   ? kIOReturnSuccess
                   : kIOReturnNotPermitted;
    }
    IOReturn initialize() override {
        auto ret = initializeHardware();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    IOReturn loadFirmware() override {
        auto ret = loadFirmwareFlow();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    IOReturn verifyReady() override {
        auto ret = verifyReadyFlow();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    IOReturn startPlayback() override { return mPlatform.commitPlayback(); }
    IOReturn stopPlayback() override {
        if (mPlatform.stopPlayback())
            return kIOReturnSuccess;
        mAmplifier.playbackFaulted = true;
        return kIOReturnNotReady;
    }
    IOReturn safeShutdown() override {
        auto ret = shutdownFlow();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    void captureDiagnostics() override { mPlatform.captureDiagnostics(); }
    IOReturn configureInterrupt(bool enabled) override {
        if (mContext.board.amplifiers[mContext.bindingIndex].interruptGpio != 2)
            return kIOReturnUnsupported;
        return mPlatform.configureInterrupt(enabled) ? kIOReturnSuccess : kIOReturnNotReady;
    }
    IOReturn handleInterrupt() override {
        if (mPlatform.checkProtection())
            return kIOReturnSuccess;
        mAmplifier.playbackFaulted = true;
        return kIOReturnNotReady;
    }
    bool managesPlaybackTransitions() const override { return true; }
    IOReturn runtimeTick(const cirrus::core::PlaybackContext& playbackContext) override {
        mPlatform.monitor(playbackContext);
        return kIOReturnSuccess;
    }
    bool playbackNeedsPreparation() const override {
        return mAmplifier.initialized && !mAmplifier.playbackActive &&
               (!mAmplifier.playbackFaulted || (mAmplifier.pllRecoveryPending && mAmplifier.pllRecoveryAttempts < 3));
    }
    bool playbackStartCancelled() const override { return mAmplifier.playbackStartCancelled; }
    bool playbackPrepared() const override {
        return mAmplifier.initialized && !mAmplifier.playbackFaulted &&
               mAmplifier.playbackTransition.phase == cirrus::devices::cs35l41::PlaybackPhase::Prepared;
    }
    bool playbackPending() const override { return mAmplifier.playbackTransition.pending() || mAmplifier.shutdownTransition.pending(); }
    bool playbackFaulted() const override {
        return mAmplifier.playbackFaulted && (!mAmplifier.pllRecoveryPending || mAmplifier.pllRecoveryAttempts >= 3);
    }
    bool validatePlayback(const cirrus::core::PlaybackContext& playbackContext) override {
        return mPlatform.validatePlayback(playbackContext);
    }
    IOReturn cancelPlayback() override {
        if (!mAmplifier.shutdownTransition.pending() && mAmplifier.cleanupAttempts < 3 &&
            (mAmplifier.playbackActive || mAmplifier.playbackTransition.pending()))
            mPlatform.cancelPlayback();
        return kIOReturnSuccess;
    }
    cirrus::core::ObservedPlayback observedPlayback() const override {
        return mAmplifier.playbackActive ? cirrus::core::ObservedPlayback::Active : cirrus::core::ObservedPlayback::Idle;
    }

private:
    bool stopAt(const char* phase) {
        if (!mPlatform.stopAfterDebugStage(phase))
            return false;
        mAmplifier.bringUpDisposition = core::BringUpDisposition::DebugHalt;
        return true;
    }
    IOReturn initializeHardware();
    IOReturn loadFirmwareFlow();
    IOReturn verifyReadyFlow();
    IOReturn shutdownFlow();
};

// Preserve the existing CS35L41 stage order and diagnostic/debug boundaries.
// Lower silicon helpers remain staged platform operations pending extraction.
IOReturn Backend::initializeHardware() {
    auto& amp = mAmplifier;
    amp.bringUpDisposition = cirrus::core::BringUpDisposition::Continue;
    amp.initialized = amp.firmwareValidated = amp.dspAlive = false;
    amp.monitorCount = 0;
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = amp.firmwareIdVersion = 0;
    if (mPlatform.channel() != cirrus::core::AudioChannel::Left && mPlatform.channel() != cirrus::core::AudioChannel::Right) {
        mPlatform.recordDiagnosticFailure(DIAG_DEVICE_ID);
        return kIOReturnNotPermitted;
    }
    if (stopAt("probe"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_PROBE);

    if (!mPlatform.perform(Operation::InitializeCodec)) {
        CIRRUS_ERR("failed to initialize codec for %s", mPlatform.name());
        if (amp.diagnostic.latestFailure == DIAG_OK)
            mPlatform.recordDiagnosticFailure(DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                              cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_OTP_BOOT);
    if (stopAt("otp"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_ERRATA);
    if (!mPlatform.perform(Operation::ApplyErrata)) {
        CIRRUS_ERR("failed to apply hardware errata for %s", mPlatform.name());
        mPlatform.recordDiagnosticFailure(DIAG_ERRATA);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_ERRATA);
    if (stopAt("errata"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_CLOCK);
    if (!mPlatform.perform(Operation::ConfigurePLL)) {
        mPlatform.recordDiagnosticFailure(DIAG_PLL_CONFIG, cirrus::devices::cs35l41::registers::kRegPllClockControl);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_CLOCK);
    if (stopAt("clock"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_ASP);
    if (!mPlatform.perform(Operation::ConfigureASP)) {
        mPlatform.recordDiagnosticFailure(DIAG_ASP_CONFIG, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_ASP);
    if (stopAt("asp"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_GPIO);
    if (!mPlatform.perform(Operation::ConfigureGPIO)) {
        mPlatform.recordDiagnosticFailure(DIAG_GPIO_CONFIG, cirrus::devices::cs35l41::registers::kRegGpioPadControl, 0x02000000);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_GPIO);
    if (stopAt("gpio"))
        return kIOReturnSuccess;

    mPlatform.setDiagnosticStage(STAGE_PLATFORM);
    if (!mPlatform.perform(Operation::ConfigureHardware)) {
        CIRRUS_ERR("platform hardware configuration failed for %s", mPlatform.name());
        mPlatform.recordDiagnosticFailure(DIAG_PLATFORM_CONFIG);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_PLATFORM);
    if (stopAt("platform"))
        return kIOReturnSuccess;

    return kIOReturnSuccess;
}

IOReturn Backend::loadFirmwareFlow() {
    auto& amp = mAmplifier;
    const bool bypass = mPlatform.bypassDSP();

    if (bypass) {
        if (!mPlatform.perform(Operation::StopDSP)) {
            mPlatform.recordDiagnosticFailure(DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
    } else {
        mPlatform.setDiagnosticStage(STAGE_FIRMWARE_DISCOVERY);
        mPlatform.discoverFirmware();
        if (amp.firmwareIdentityPending) {
            amp.bringUpDisposition = cirrus::core::BringUpDisposition::Wait;
            return kIOReturnSuccess;
        }
        if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
            mPlatform.recordDiagnosticFailure(DIAG_FIRMWARE_MISSING, 0, 1, 0);
            return kIOReturnNotReady;
        }
        mPlatform.markDiagnosticSuccess(STAGE_FIRMWARE_DISCOVERY);
        mPlatform.setDiagnosticStage(STAGE_FIRMWARE_UPLOAD);
        mPlatform.initializeFirmware();
        if (mPlatform.firmwarePhaseHalted() && amp.firmwareValidated && !amp.playbackFaulted) {
            amp.bringUpDisposition = cirrus::core::BringUpDisposition::DebugHalt;
            return kIOReturnSuccess;
        }
        if (amp.playbackFaulted || !amp.firmwareValidated || !amp.dspAlive || amp.monitorCount != 1) {
            if (amp.diagnostic.latestFailure == DIAG_OK)
                mPlatform.recordDiagnosticFailure(DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
        mPlatform.markDiagnosticSuccess(STAGE_DSP_BOOT);
    }

    if ((bypass && stopAt("firmware")) || stopAt("dsp")) {
        // Firmware initialization includes DSP boot; explicitly halt it before returning.
        if (!mPlatform.perform(Operation::StopDSP)) {
            amp.playbackFaulted = true;
            mPlatform.recordDiagnosticFailure(DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
        return kIOReturnSuccess;
    }

    return kIOReturnSuccess;
}

IOReturn Backend::verifyReadyFlow() {
    auto& amp = mAmplifier;
    const bool bypass = mPlatform.bypassDSP();

    mPlatform.setDiagnosticStage(STAGE_IDLE_VERIFY);
    if (!mPlatform.perform(Operation::PowerUp)) {
        mPlatform.recordDiagnosticFailure(DIAG_IDLE_ROLLBACK);
        return kIOReturnNotReady;
    }
    if (!mPlatform.perform(Operation::VerifyIdle)) {
        CIRRUS_ERR("idle hardware verification failed for %s", mPlatform.name());
        mPlatform.recordDiagnosticFailure(DIAG_IDLE_INVARIANT);
        return kIOReturnNotReady;
    }
    if (!bypass && !mPlatform.perform(Operation::VerifyDSP)) {
        mPlatform.recordDiagnosticFailure(DIAG_DSP_BOOT);
        return kIOReturnNotReady;
    }
    mPlatform.markDiagnosticSuccess(STAGE_SAFE_IDLE);
    mPlatform.logASPSnapshot();

    amp.initialized = true;
    return kIOReturnSuccess;
}

IOReturn Backend::shutdownFlow() {
    auto& amp = mAmplifier;

    bool clean = true;
    amp.initialized = amp.firmwareValidated = false;

    if (amp.present) {
        if (!mPlatform.stopPlayback()) {
            clean = false;
            mPlatform.recordDiagnosticFailure(DIAG_IDLE_ROLLBACK);
        }
        if (!mPlatform.perform(Operation::StopDSP)) {
            clean = false;
            mPlatform.recordDiagnosticFailure(DIAG_DSP_BOOT);
        }
    }
    amp.dspAlive = false;
    amp.monitorCount = 0;
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;
    return clean ? kIOReturnSuccess : kIOReturnNotReady;
}

}

cirrus::core::AmplifierLifecycleHooks* cirrus::devices::cs35l41::createBackend(const cirrus::platform::AmplifierBackendContext& context) {
    auto* platform = static_cast<Platform*>(context.platformContext);
    if (!platform || !platform->state() || !context.board.amplifiers || context.bindingIndex >= context.board.count)
        return nullptr;
    const auto& binding = context.board.amplifiers[context.bindingIndex];
    if (platform->model() != core::AmplifierModel::CS35L41 || binding.siliconId != registers::kValDeviceId ||
        !platform->endpoint().sameDevice(context.endpoint) || !context.endpoint.sameDevice(binding.endpoint) ||
        uint32_t(platform->channel()) != binding.channelIndex ||
        (platform->channel() != core::AudioChannel::Left && platform->channel() != core::AudioChannel::Right))
        return nullptr;
    return new Backend(context, *platform);
}
void cirrus::devices::cs35l41::destroyBackend(cirrus::core::AmplifierLifecycleHooks* backend) { delete backend; }
