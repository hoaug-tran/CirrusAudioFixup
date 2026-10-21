#include "Devices/Amplifiers/CS35L41/CS35L41Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
bool CS35L41Driver::configureCS35L41Interrupt(AmplifierState& amp, bool enabled) {
    if (!enabled)
        return writeRegister(amp, 0x10110, 0xFFFFFFFF, TRACE_PLAYBACK);
    if (amp.gpio2Function != 2)
        return false;
    return amp.initialized && !amp.playbackFaulted && checkProtectionStatus(amp) &&
           updateRegisterBits(amp, 0x242C, 0x07000000, 0x02000000, TRACE_PLAYBACK) &&
           updateRegisterBits(amp, 0x1100C, 0x80001000, 0x80000000, TRACE_PLAYBACK) &&
           writeRegister(amp, 0x10110, 0x7FFD7E3F, TRACE_PLAYBACK);
}

bool CS35L41Driver::checkProtectionStatus(AmplifierState& amp) {
    uint32_t status = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kIrq1Status1Register, &status, TRACE_PLAYBACK))
        return false;
    char prop[80];
    snprintf(prop, sizeof(prop), "Cirrus_Protection_Fault_%s", amp.name);
    if (status & cirrus::devices::amplifiers::cs35l41::registers::kMaskProtectionFault) {
        const char* faultReason = "UNKNOWN_FAULT";
        if (status & 0x00000080) {
            faultReason = "CRITICAL_OVERTEMPERATURE";
        } else if (status & 0x80000000) {
            faultReason = "SPEAKER_SHORT_CIRCUIT";
        } else if (status & 0x00020000) {
            faultReason = "BOOST_OVERVOLTAGE";
        } else if (status & 0x00008000) {
            faultReason = "BOOST_SHORT_CIRCUIT";
        } else if (status & 0x00000040) {
            faultReason = "BOOST_PEAK_CURRENT_LIMIT";
        } else if (status & 0x00000100) {
            faultReason = "TEMPERATURE_WARNING";
        }
        OSString* faultVal = OSString::withCString(faultReason);
        if (faultVal) {
            setProperty(prop, faultVal);
            faultVal->release();
        }
        recordDiagnosticFailure(amp, DIAG_AMP_PROTECTION, cirrus::devices::amplifiers::cs35l41::registers::kIrq1Status1Register, 0, status);
        return false;
    }
    OSString* okVal = OSString::withCString("OK");
    if (okVal) {
        setProperty(prop, okVal);
        okVal->release();
    }
    return true;
}

bool CS35L41Driver::stopPlayback(AmplifierState& amp) {
    if (amp.model != cirrus::core::AmplifierModel::CS35L41) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
        return false;
    }
    // Cancel preparation before cleanup so no later callback can commit it.
    amp.playbackTransition = {};
    amp.shutdownTransition = {};
    amp.clockWaitAfterCleanup = false;
    ++amp.cleanupAttempts;
    setDiagnosticStage(amp, STAGE_PLAYBACK_CLEANUP);
    amp.playbackStableCount = 0;
    bool ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, 0, TRACE_PLAYBACK);
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678,
                       TRACE_PLAYBACK) &&
         ok;
    uint32_t before = 0;
    bool known = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl1, &before, TRACE_PLAYBACK);
    bool needPdn = !known || (before & 1);
    ok = known && ok;
    bool unlocked = !amp.power.internal() && unlockTestKey(amp);
    ok = (amp.power.internal() || unlocked) && ok;
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00585941, TRACE_PLAYBACK) && ok;
    if (needPdn)
        ok = writeRegister(amp, 0x00010010, 0x00800000, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl1, 1, 0, TRACE_PLAYBACK) && ok;
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
        IOSleep(1);
    }
    if (!pdnDone)
        recordDiagnosticFailure(amp, DIAG_POWER_DOWN_TIMEOUT, 0x00010010, 0x00800000, irq);
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00580941, TRACE_PLAYBACK) && ok;
    if (!amp.power.internal()) {
        ok = lockTestKey(amp) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegGpio1Control1, 0x00000001, TRACE_PLAYBACK) && ok;
    }
    ok = updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2, 1, 0, TRACE_PLAYBACK) && ok;
    if (amp.dspAlive) {
        bool paused = sendMailboxCommand(amp, cirrus::devices::amplifiers::cs35l41::registers::kCmdMailboxPause,
                                         cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxPaused);
        ok = paused && ok;
        if (!paused) {
            amp.firmwareValidated = false;
            stopDSP(amp);
        }
    }
    ok = updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2,
                            0x00003000 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable, 0, TRACE_PLAYBACK) &&
         ok;
    uint32_t pwr1 = 0, pwr2 = 0, volume = 0, gain = 0;
    bool readable =
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl1, &pwr1, TRACE_PLAYBACK) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &volume, TRACE_PLAYBACK) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PLAYBACK);
    bool safe = ok && pdnDone && readable && !(pwr1 & 1) &&
                !(pwr2 & (0x00003001 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable)) && volume == 0x0000A678 &&
                gain == 0;
    amp.playbackActive = !safe;
    if (!safe) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2, 0, pwr2);
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

bool CS35L41Driver::stopRuntimePlayback(AmplifierState& amp) {
    using namespace cirrus::devices::amplifiers::cs35l41;
    if (amp.model != cirrus::core::AmplifierModel::CS35L41) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
        return false;
    }
    auto& state = amp.shutdownTransition;
    if (!state.pending()) {
        state = {};
        amp.playbackTransition = {};
        ++amp.cleanupAttempts;
        amp.playbackStableCount = 0;
        setDiagnosticStage(amp, STAGE_PLAYBACK_CLEANUP);
    }
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    DriverRegisterIO io(this, amp, TRACE_PLAYBACK);
    auto result = Playback::shutdown(io, state, now / 1000000, amp.dspAlive, amp.firmwareIdVersion, playbackTimeMilliseconds, amp.power);
    if (result == PlaybackProgress::Pending) {
        // This flag means cleanup is unverified, not that audio is unmuted.
        amp.playbackActive = true;
        return false;
    }
    if (state.haltDsp) {
        amp.firmwareValidated = false;
        stopDSP(amp);
    }
    bool safe = result == PlaybackProgress::Prepared;
    amp.playbackActive = !safe;
    if (safe && amp.clockWaitAfterCleanup && amp.diagnostic.latestFailure == DIAG_POWER_UP_TIMEOUT) {
        if (amp.pllRecoveryAttempts > 0)
            --amp.pllRecoveryAttempts;
        amp.pllRecoveryPending = true;
        amp.pllRetryCooldown = 20;
        recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, registers::kRegIrq1RawStatus3, 2, 0);
    }
    amp.clockWaitAfterCleanup = false;
    if (!safe) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, state.failure, state.failureRegister, 0, state.actual, mLastTransferReturn);
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

bool CS35L41Driver::validateCS35L41Playback(AmplifierState& amp, const cirrus::core::PlaybackContext& context) {
    if (amp.playbackTransition.streamTag != mHdaState.lastStreamTag || amp.playbackTransition.streamFormat != mHdaState.lastFormat ||
        (mAudioEvents && !mAudioEvents->unchanged(context.generation)))
        return false;
    uint32_t pll = 0;
    bool readable = checkProtectionStatus(amp) &&
                    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1RawStatus3, &pll, TRACE_PLAYBACK);
    if (readable && (pll & 2))
        return true;
    amp.playbackFaulted = true;
    if (readable) {
        amp.pllRecoveryPending = true;
        recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1RawStatus3, 2, pll);
    }
    return false;
}

IOReturn CS35L41Driver::commitCS35L41Playback(AmplifierState& amp) {
    using namespace cirrus::devices::amplifiers::cs35l41;
    amp.playbackStartCancelled = false;
    if (!synchronizeHdaStream() || !mHdaState.observed || amp.playbackTransition.streamTag != mHdaState.lastStreamTag ||
        amp.playbackTransition.streamFormat != mHdaState.lastFormat ||
        (mAudioEvents && !mAudioEvents->unchanged(amp.playbackTransition.generation))) {
        amp.playbackStartCancelled = true;
        return kIOReturnNotReady;
    }
    if (amp.playbackTransition.phase != PlaybackPhase::Prepared)
        return kIOReturnNotReady;
    DriverRegisterIO io(this, amp, TRACE_PLAYBACK);
    uint32_t gain = amp.monitorCount >= 1 ? tuning::encodeGain(amp.tuningPcmGain) : registers::kPlaybackBypassGain;
    if (!Playback::commit(io, amp.playbackTransition, gain, amp.monitorCount >= 1, amp.power)) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, amp.playbackTransition.failure);
        return kIOReturnNotReady;
    }
    amp.playbackActive = true;
    amp.pllRecoveryAttempts = 0;
    amp.pllRecoveryPending = false;
    markDiagnosticSuccess(amp, STAGE_PLAYBACK_ACTIVE);
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Playback_PrepareMs_%s", amp.name);
    setProperty(property, playbackTimeMilliseconds() - amp.playbackTransition.prepareStartedMs, 64);
    snprintf(property, sizeof(property), "Cirrus_Playback_Verdict_%s", amp.name);
    setProperty(property, "ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED");
    return kIOReturnSuccess;
}

void CS35L41Driver::monitorCS35L41(AmplifierState& amp, const cirrus::core::PlaybackContext& context) {
    const bool hasAudio = context.hasAudio, eventRoute = context.eventRoute, routePermitsPlayback = context.routePermitsPlayback;
    const bool hdaStreamActive = context.streamActive;
    const uint32_t routeGeneration = context.generation;
    const uint64_t nowMs = context.nowMs;
    if (!amp.present)
        return;
    if (amp.model != cirrus::core::AmplifierModel::CS35L41) {
        // Initialization dispatch is not enough: runtime must also keep
        // unimplemented chips away from CS35L41 register transactions.
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, 0, 0, 0, kIOReturnNotReady, false);
        return;
    }
    if (amp.shutdownTransition.pending()) {
        stopRuntimePlayback(amp);
        return;
    }
    // Reprepare when the active stream changes, even without an IOAudio event.
    if (((amp.playbackActive && !amp.playbackFaulted) || amp.playbackTransition.pending()) &&
        (!hasAudio || amp.playbackTransition.generation != routeGeneration || amp.playbackTransition.streamTag != mHdaState.lastStreamTag ||
         amp.playbackTransition.streamFormat != mHdaState.lastFormat)) {
        stopRuntimePlayback(amp);
        return;
    }
    // A completed output/engine change is normal lifecycle input, not a
    // hardware fault. Stop immediately instead of waiting for two timer
    // observations or manufacturing PLL errors while headphones play.
    if (eventRoute && !routePermitsPlayback) {
        if (amp.playbackActive && amp.cleanupAttempts < 3)
            stopRuntimePlayback(amp);
        if (amp.pllRecoveryPending && amp.diagnostic.latestFailure == DIAG_PLL_UNLOCKED && !amp.playbackActive) {
            amp.playbackFaulted = false;
            amp.pllRecoveryPending = false;
            amp.pllRecoveryAttempts = 0;
        }
        return;
    }
    if (amp.playbackFaulted) {
        if (amp.playbackActive && amp.cleanupAttempts < 3)
            stopRuntimePlayback(amp);
        if (!amp.playbackActive && hasAudio && amp.initialized && amp.pllRecoveryPending && amp.pllRecoveryAttempts < 3 &&
            amp.diagnostic.latestFailure == DIAG_PLL_UNLOCKED && checkProtectionStatus(amp)) {
            // The selected speaker route authorizes preparation. Idle PLL
            // status is not a readiness gate: this hardware can report
            // unlocked until GLOBAL_EN starts the power-up sequence.
            // Only the compatibility fallback retains timeout backoff;
            // event-driven route changes never inherit headphone cooldown.
            if (!eventRoute && amp.pllRetryCooldown > 0) {
                --amp.pllRetryCooldown;
                return;
            }
            amp.pllRetryCooldown = 0;
            amp.playbackFaulted = false;
            amp.pllRecoveryPending = false;
            ++amp.pllRecoveryAttempts;
            amp.cleanupAttempts = 0;
        } else {
            return;
        }
    }

    if (amp.initialized && !amp.playbackActive && mHdaState.observed && !hdaStreamActive) {
        amp.playbackStableCount = 0;
        amp.pllRetryCooldown = 0;
        return;
    }

    // Preparation already checks status in its chip-specific phases and
    // the stereo commit revalidates every safety invariant. Avoid the
    // active-health/logging sweep on each 1 ms preparation callback.
    if (amp.playbackTransition.pending()) {
        if (!amp.initialized || !checkProtectionStatus(amp)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }
    } else {
        uint32_t pwrCtrl1 = 0;
        uint32_t pllLockSts = 0;
        uint32_t mbox2 = 0;
        uint32_t timestamp = 0;

        if (!readRegister(amp, 0x00002014, &pwrCtrl1, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }

        bool pllReadable = readRegister(amp, 0x00010098, &pllLockSts, TRACE_DUMP);
        if (!pllReadable) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }
        bool globalEn = (pwrCtrl1 & 0x01) != 0;
        bool pllLock = pllReadable && (pllLockSts & 0x00000002) != 0;

        if (!readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }

        if (!amp.initialized) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }

        if (!checkProtectionStatus(amp)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }

        if (gCirrusDebug && !readRegister(amp, 0x025C0800, &timestamp, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopRuntimePlayback(amp);
            return;
        }

        if (amp.playbackActive && hasAudio) {
            const bool dspMode = amp.monitorCount >= 1;
            uint32_t pwr2 = 0, core = 0, halo = 0;
            bool powerReadable = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK);
            bool dspHealthy = bootArgEnabled("-cirrusnodsp");
            if (dspMode) {
                dspHealthy =
                    amp.dspAlive && amp.firmwareValidated && amp.haloStateRegister &&
                    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &core, TRACE_PLAYBACK) &&
                    readRegister(amp, amp.haloStateRegister, &halo, TRACE_PLAYBACK) &&
                    (core & (cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable |
                             cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset)) ==
                        cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable &&
                    halo == 2 && mbox2 == cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxRunning;
            }
            if (!globalEn || !pllLock || !powerReadable ||
                (pwr2 & (0x3001 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable)) != amp.power.activePower(dspMode) ||
                !dspHealthy) {
                CIRRUS_ERR("ACTIVE_STATE_LOST amp=%s pwr1=0x%08X pwr2=0x%08X pll=%d mbox=%u core=0x%08X halo=%u", amp.name, pwrCtrl1, pwr2,
                           pllLock, mbox2, core, halo);
                if (!pllLock) {
                    recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1RawStatus3, 2,
                                            pllLockSts);
                    amp.pllRetryCooldown = 4;
                    amp.pllRecoveryPending = globalEn && powerReadable && dspHealthy &&
                                             (pwr2 & (0x3001 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable)) ==
                                                 amp.power.activePower(dspMode);
                } else {
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2,
                                            amp.power.activePower(dspMode), pwr2);
                }
                amp.playbackFaulted = true;
                stopRuntimePlayback(amp);
                return;
            }
        }

        bool playbackTransition = hasAudio != amp.playbackActive;
        bool heartbeat = (++amp.monitorLogCountdown >= (hasAudio ? 50U : 40U));
        if (gCirrusDebug && (playbackTransition || heartbeat)) {
            uint32_t aspEnables = 0, spRate = 0, spFmt = 0, dacSrc = 0, ampVol = 0;
            bool snapshotReadable =
                readRegister(amp, 0x00004800, &aspEnables, TRACE_DUMP) && readRegister(amp, 0x00004804, &spRate, TRACE_DUMP) &&
                readRegister(amp, 0x00004808, &spFmt, TRACE_DUMP) && readRegister(amp, 0x00004C00, &dacSrc, TRACE_DUMP) &&
                readRegister(amp, 0x00006000, &ampVol, TRACE_DUMP);
            if (!snapshotReadable) {
                amp.playbackFaulted = true;
                stopRuntimePlayback(amp);
                return;
            }

            CIRRUS_LOG("background monitor %s: global_en=%d pll_lock=%d mbox2=0x%08X ts=0x%08X->0x%08X audio=%d active=%d "
                       "hda_sd=%u tag=%u fmt=0x%04X sp_en=0x%08X sp_rate=0x%08X sp_fmt=0x%08X dac_src=0x%08X vol=0x%08X mode=%s",
                       amp.name, globalEn, pllLock, mbox2, amp.lastTimestamp, timestamp, hasAudio, amp.playbackActive,
                       mHdaState.lastDescriptor, mHdaState.lastStreamTag, mHdaState.lastFormat, aspEnables, spRate, spFmt, dacSrc, ampVol,
                       (amp.monitorCount >= 1) ? "DSP" : "BYPASS");
            amp.monitorLogCountdown = 0;
        }

        amp.lastTimestamp = timestamp;
    }

    if (hasAudio && !amp.playbackActive) {
        using namespace cirrus::devices::amplifiers::cs35l41;
        auto& transition = amp.playbackTransition;
        const bool dspMode = amp.monitorCount >= 1;
        if (amp.model != cirrus::core::AmplifierModel::CS35L41 || (!dspMode && !bootArgEnabled("-cirrusnodsp")) ||
            (dspMode && (!amp.dspAlive || !amp.firmwareValidated))) {
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT);
            stopRuntimePlayback(amp);
            return;
        }
        if (transition.phase == PlaybackPhase::Idle) {
            transition.generation = routeGeneration;
            transition.streamTag = mHdaState.lastStreamTag;
            transition.streamFormat = mHdaState.lastFormat;
            setDiagnosticStage(amp, STAGE_PLAYBACK_PREPARE);
        }
        DriverRegisterIO io(this, amp, TRACE_PLAYBACK);
        auto result = Playback::advance(io, transition, nowMs, dspMode, amp.firmwareIdVersion, playbackTimeMilliseconds, amp.power);
        if (result == PlaybackProgress::Failed) {
            const auto failure = transition.failure;
            recordDiagnosticFailure(amp, failure, transition.failureRegister, transition.expected, transition.actual, mLastTransferReturn);
            uint32_t retryPllStatus = 0;
            bool clockWait = failure == DIAG_POWER_UP_TIMEOUT && amp.pllRecoveryAttempts > 0 &&
                             readRegister(amp, registers::kRegIrq1RawStatus3, &retryPllStatus, TRACE_PLAYBACK) && !(retryPllStatus & 2) &&
                             checkProtectionStatus(amp);
            amp.playbackFaulted = true;
            amp.pllRecoveryPending = failure == DIAG_PLL_UNLOCKED;
            amp.pllRetryCooldown = 4;
            amp.clockWaitAfterCleanup = clockWait;
            stopRuntimePlayback(amp);
        }
    } else if (!hasAudio && amp.playbackActive) {
        amp.playbackStableCount++;
        if (amp.playbackStableCount >= 2) {
            stopRuntimePlayback(amp);
            amp.pllRetryCooldown = 0;
        }
    } else {
        amp.playbackStableCount = 0;
    }
}
}
