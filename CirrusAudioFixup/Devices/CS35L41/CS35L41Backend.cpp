// CS35L41 lifecycle backend. The IOService lends serialized platform resources
// and register ports; concrete silicon sequences remain CS35L41-specific.
#include "CirrusAudioFixup.hpp"
using namespace cirrus::diagnostics;

class CirrusAudioFixup::CS35L41Backend final : public cirrus::core::AmplifierLifecycleHooks {
    CirrusAudioFixup& mOwner;
    AmplifierState& mAmplifier;
    const cirrus::platform::AmplifierBackendContext& mContext;
public:
    explicit CS35L41Backend(const cirrus::platform::AmplifierBackendContext& c)
        :mOwner(*static_cast<AmplifierBackendOwner*>(c.platformContext)->owner),
         mAmplifier(*static_cast<AmplifierBackendOwner*>(c.platformContext)->amp),mContext(c) {}
    cirrus::core::BringUpDisposition disposition() const override {return mAmplifier.bringUpDisposition;}
    IOReturn identify() override {
        uint32_t id=0;
        if(!mContext.io.read(cirrus::devices::cs35l41::registers::kRegDeviceId,&id) ||
           id!=cirrus::devices::cs35l41::registers::kValDeviceId) {
            mAmplifier.playbackFaulted=true;mOwner.recordDiagnosticFailure(mAmplifier,DIAG_DEVICE_ID);return kIOReturnNotFound;
        }
        return kIOReturnSuccess;
    }
    IOReturn authorizeBoard() override {
        return mOwner.mPlatformAuthorized && mContext.board.permits(mContext.actualBoard) &&
            (mAmplifier.channel==cirrus::core::AudioChannel::Left || mAmplifier.channel==cirrus::core::AudioChannel::Right)
            ?kIOReturnSuccess:kIOReturnNotPermitted;
    }
    IOReturn initialize() override {auto ret=mOwner.initializeCS35L41Hardware(mAmplifier);if(ret!=kIOReturnSuccess)mAmplifier.playbackFaulted=true;return ret;}
    IOReturn loadFirmware() override {auto ret=mOwner.loadCS35L41Firmware(mAmplifier);if(ret!=kIOReturnSuccess)mAmplifier.playbackFaulted=true;return ret;}
    IOReturn verifyReady() override {auto ret=mOwner.verifyCS35L41Ready(mAmplifier);if(ret!=kIOReturnSuccess)mAmplifier.playbackFaulted=true;return ret;}
    IOReturn startPlayback() override {return mOwner.commitCS35L41Playback(mAmplifier);}
    IOReturn stopPlayback() override {if(mOwner.stopPlayback(mAmplifier))return kIOReturnSuccess;mAmplifier.playbackFaulted=true;return kIOReturnNotReady;}
    IOReturn safeShutdown() override {auto ret=mOwner.shutdownCS35L41(mAmplifier);if(ret!=kIOReturnSuccess)mAmplifier.playbackFaulted=true;return ret;}
    void captureDiagnostics() override {mOwner.captureCS35L41FailureSnapshot(mAmplifier,mAmplifier.diagnostic.latestFailure);}
    IOReturn configureInterrupt(bool enabled) override {
        if(mContext.board.amplifiers[mContext.bindingIndex].interruptGpio!=2)return kIOReturnUnsupported;
        return mOwner.configureCS35L41Interrupt(mAmplifier,enabled)?kIOReturnSuccess:kIOReturnNotReady;
    }
    IOReturn handleInterrupt() override {
        if(mOwner.checkProtectionStatus(mAmplifier))return kIOReturnSuccess;
        mAmplifier.playbackFaulted=true;return kIOReturnNotReady;
    }
    bool managesPlaybackTransitions() const override {return true;}
    IOReturn runtimeTick(const cirrus::core::PlaybackContext& playbackContext) override {mOwner.monitorCS35L41(mAmplifier,playbackContext);return kIOReturnSuccess;}
    bool playbackNeedsPreparation() const override {return mAmplifier.initialized && !mAmplifier.playbackActive && (!mAmplifier.playbackFaulted || (mAmplifier.pllRecoveryPending && mAmplifier.pllRecoveryAttempts<3));}
    bool playbackStartCancelled() const override {return mAmplifier.playbackStartCancelled;}
    bool playbackPrepared() const override {return mAmplifier.initialized && !mAmplifier.playbackFaulted && mAmplifier.playbackTransition.phase==cirrus::devices::cs35l41::PlaybackPhase::Prepared;}
    bool playbackPending() const override {return mAmplifier.playbackTransition.pending() || mAmplifier.shutdownTransition.pending();}
    bool playbackFaulted() const override {return mAmplifier.playbackFaulted && (!mAmplifier.pllRecoveryPending || mAmplifier.pllRecoveryAttempts>=3);}
    bool validatePlayback(const cirrus::core::PlaybackContext& playbackContext) override {return mOwner.validateCS35L41Playback(mAmplifier,playbackContext);}
    IOReturn cancelPlayback() override {
        if(!mAmplifier.shutdownTransition.pending() && mAmplifier.cleanupAttempts<3 && (mAmplifier.playbackActive || mAmplifier.playbackTransition.pending()))mOwner.stopRuntimePlayback(mAmplifier);
        return kIOReturnSuccess;
    }
    cirrus::core::ObservedPlayback observedPlayback() const override {return mAmplifier.playbackActive?cirrus::core::ObservedPlayback::Active:cirrus::core::ObservedPlayback::Idle;}

};

cirrus::core::AmplifierLifecycleHooks* cirrus::devices::cs35l41::createBackend(const cirrus::platform::AmplifierBackendContext& context) {
    auto* binding=static_cast<CirrusAudioFixup::AmplifierBackendOwner*>(context.platformContext);
    if(!binding || !binding->owner || !binding->amp || binding->amp->model!=cirrus::core::AmplifierModel::CS35L41)return nullptr;
    return new CirrusAudioFixup::CS35L41Backend(context);
}
void cirrus::devices::cs35l41::destroyBackend(cirrus::core::AmplifierLifecycleHooks* backend){delete backend;}
