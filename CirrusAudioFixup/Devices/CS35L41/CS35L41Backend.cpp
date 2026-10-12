// CS35L41 lifecycle backend. The IOService lends serialized platform resources
// and register ports; concrete silicon sequences remain CS35L41-specific.
#include "Devices/CS35L41/CS35L41Backend.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
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
            mPlatform.recordIdentityFailure(id, result);
            return result;
        }
        if (id != registers::kValDeviceId) {
            mAmplifier.playbackFaulted = true;
            mPlatform.recordIdentityFailure(id, kIOReturnNotFound);
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
        auto ret = mPlatform.initialize();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    IOReturn loadFirmware() override {
        auto ret = mPlatform.loadFirmware();
        if (ret != kIOReturnSuccess)
            mAmplifier.playbackFaulted = true;
        return ret;
    }
    IOReturn verifyReady() override {
        auto ret = mPlatform.verifyReady();
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
        auto ret = mPlatform.shutdown();
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
};

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
