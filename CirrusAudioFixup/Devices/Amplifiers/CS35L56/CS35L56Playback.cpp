#include "Devices/Amplifiers/CS35L56/CS35L56Internal.hpp"
#include "Platform/HDA/HDAStreamWatcher.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
IOReturn CS35L56Driver::checkProtectionStatus() {
    uint32_t shortCircuit = 0, temperature = 0;
    if (!mContext.io.read(registers::kRegIrqFault1, &shortCircuit) || !mContext.io.read(registers::kRegIrqFault8, &temperature))
        return transferFailure(diagnostics::DIAG_AMP_PROTECTION);
    if ((shortCircuit | temperature) & registers::kProtectionFaultMask)
        return fail(kIOReturnNotReady, diagnostics::DIAG_AMP_PROTECTION);
    return kIOReturnSuccess;
}
bool CS35L56Driver::validatePlayback(const core::PlaybackContext& context) {
    const auto stream = mHost.streamObservation();
    // The HDA configuration is the Linux 48 kHz, stereo, 24-bit I2S contract.
    return writable() && mAmplifier.initialized && mAmplifier.firmwareValidated && mAmplifier.calibrated && !mAmplifier.faulted &&
           context.hasAudio && context.eventRoute && context.routePermitsPlayback && context.streamActive && context.streamTag &&
           platform::hda::HDAStreamWatcher::isFormatSupported(context.streamFormat) && stream.observed && stream.streamActive &&
           stream.converterPrepared && stream.lastStreamTag == context.streamTag && stream.lastFormat == context.streamFormat &&
           mHost.hasRouteEvents() && mHost.routeUnchanged(context.generation);
}
IOReturn CS35L56Driver::runtimeTick(const core::PlaybackContext& context) {
    mAmplifier.routePrepared = validatePlayback(context);
    if (mAmplifier.routePrepared)
        mAmplifier.routeGeneration = context.generation;
    if (!mAmplifier.playbackActive)
        return kIOReturnSuccess;
    if (!mAmplifier.routePrepared)
        return stopPlayback();
    const auto protection = checkProtectionStatus();
    if (protection != kIOReturnSuccess) {
        stopPlayback();
        return protection;
    }
    uint32_t version = 0, halo = 0;
    if (!mContext.io.read(registers::kRegFirmwareVersion, &version) || !mContext.io.read(registers::kRegDspHaloState, &halo)) {
        const auto result = transferFailure(diagnostics::DIAG_DSP_BOOT);
        stopPlayback();
        return result;
    }
    if (version != registers::kFirmwareVersion || halo < 2 || halo >= 0xFFFF) {
        const auto result = fail(kIOReturnNotReady, diagnostics::DIAG_DSP_BOOT);
        stopPlayback();
        return result;
    }
    return kIOReturnSuccess;
}
IOReturn CS35L56Driver::startPlayback() {
    using namespace registers;
    if (!writable() || !mAmplifier.initialized || !mAmplifier.calibrated || mAmplifier.faulted || !mAmplifier.routePrepared ||
        !mHost.routeUnchanged(mAmplifier.routeGeneration))
        return kIOReturnNotPermitted;
    if (mAmplifier.playbackActive)
        return kIOReturnSuccess;
    struct PlaybackCleanup {
        CS35L56Driver& driver;
        bool committed{false};
        ~PlaybackCleanup() {
            if (!committed)
                driver.stopPlayback();
        }
    } cleanup{*this};
    // Revalidate after idle or possible power loss before any AUDIO_PLAY.
    const auto verified = verifyReady();
    if (verified != kIOReturnSuccess)
        return verified;
    const auto protection = checkProtectionStatus();
    if (protection != kIOReturnSuccess)
        return protection;
    if (!mHost.routeUnchanged(mAmplifier.routeGeneration))
        return kIOReturnNotReady;
    if (!DSPControl::sendMailbox(mContext.io, kCommandAudioPlay, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT);
    uint32_t state = 0;
    bool ready = false;
    for (uint32_t elapsed = 0; elapsed <= kPS0TimeoutUs; elapsed += kPS0PollUs) {
        if (!mContext.io.read(kRegTransducerState, &state))
            return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegTransducerState);
        if (state == 0) {
            ready = true;
            break;
        }
        if (elapsed < kPS0TimeoutUs)
            delayUs(kPS0PollUs);
    }
    if (!ready)
        return fail(kIOReturnTimeout, diagnostics::DIAG_DSP_BOOT, kRegTransducerState);
    if (!mHost.routeUnchanged(mAmplifier.routeGeneration)) {
        stopPlayback();
        return kIOReturnNotReady;
    }
    const uint32_t aspMask = kAspReceiveMask | (1U << mContext.endpoint.spiChipSelect);
    uint32_t enables = 0, mute = 1;
    if (!mContext.io.write(kRegAspEnables, aspMask) || !mContext.io.read(kRegAspEnables, &enables))
        return transferFailure(diagnostics::DIAG_PLAYBACK_INVARIANT);
    if (enables != aspMask || !mHost.routeUnchanged(mAmplifier.routeGeneration))
        return fail(kIOReturnNotReady, diagnostics::DIAG_PLAYBACK_INVARIANT);
    if (!mContext.io.write(kRegUserMute, 0) || !mContext.io.read(kRegUserMute, &mute))
        return transferFailure(diagnostics::DIAG_PLAYBACK_INVARIANT);
    if (enables != aspMask || mute || !mHost.routeUnchanged(mAmplifier.routeGeneration))
        return fail(kIOReturnNotReady, diagnostics::DIAG_PLAYBACK_INVARIANT);
    mAmplifier.playbackActive = true;
    cleanup.committed = true;
    publishState("PLAYBACK_ACTIVE_HARDWARE_UNVERIFIED");
    return kIOReturnSuccess;
}
IOReturn CS35L56Driver::stopPlayback() {
    using namespace registers;
    mAmplifier.routePrepared = false;
    if (!mAmplifier.hardwareTouched)
        return kIOReturnSuccess;
    if (!writable())
        return kIOReturnNotPermitted;
    IOReturn result = kIOReturnSuccess;
    auto failed = [&](uint32_t reg) {
        const auto error = transferFailure(diagnostics::DIAG_IDLE_ROLLBACK, reg);
        if (result == kIOReturnSuccess)
            result = error;
    };
    // Attempt each mute/disable step even if an earlier cleanup transfer failed.
    if (!mContext.io.write(kRegUserMute, 1))
        failed(kRegUserMute);
    if (!mContext.io.write(kRegAspEnables, 0))
        failed(kRegAspEnables);
    if (mAmplifier.booted && !DSPControl::sendMailbox(mContext.io, kCommandAudioPause, delayUs))
        failed(kRegDspVirtualMailbox1);
    uint32_t mute = 0, enables = kAspReceiveMask;
    if (!mContext.io.read(kRegUserMute, &mute))
        failed(kRegUserMute);
    else if (mute != 1) {
        const auto error = fail(kIOReturnNotReady, diagnostics::DIAG_IDLE_ROLLBACK, kRegUserMute);
        if (result == kIOReturnSuccess)
            result = error;
    }
    if (!mContext.io.read(kRegAspEnables, &enables))
        failed(kRegAspEnables);
    else if (enables) {
        const auto error = fail(kIOReturnNotReady, diagnostics::DIAG_IDLE_ROLLBACK, kRegAspEnables);
        if (result == kIOReturnSuccess)
            result = error;
    }
    if (result == kIOReturnSuccess)
        mAmplifier.playbackActive = false;
    return result;
}
}
