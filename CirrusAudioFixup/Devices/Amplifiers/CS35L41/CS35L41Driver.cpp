#include "Devices/Amplifiers/CS35L41/CS35L41Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
IOReturn CS35L41Driver::initializeHardware() {
    auto& amp = mAmplifier;
    amp.bringUpDisposition = cirrus::core::BringUpDisposition::Continue;
    amp.initialized = amp.firmwareValidated = amp.dspAlive = false;
    amp.monitorCount = 0;
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = amp.firmwareIdVersion = 0;
    if (amp.channel != cirrus::core::AudioChannel::Left && amp.channel != cirrus::core::AudioChannel::Right) {
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID);
        return kIOReturnNotPermitted;
    }
    if (stopAt("probe"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_PROBE);

    if (!initializeCS35L41Device(amp)) {
        CIRRUS_ERR("failed to initialize codec for %s", amp.name);
        if (amp.diagnostic.latestFailure == DIAG_OK)
            recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId,
                                    cirrus::devices::amplifiers::cs35l41::registers::kValDeviceId, amp.deviceId);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_OTP_BOOT);
    if (stopAt("otp"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_ERRATA);
    if (!initializeHardwareErrata(amp)) {
        CIRRUS_ERR("failed to apply hardware errata for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_ERRATA);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_ERRATA);
    if (stopAt("errata"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_CLOCK);
    if (!applyPLL(amp)) {
        recordDiagnosticFailure(amp, DIAG_PLL_CONFIG, cirrus::devices::amplifiers::cs35l41::registers::kRegPllClockControl);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_CLOCK);
    if (stopAt("clock"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_ASP);
    if (!applyASP(amp)) {
        recordDiagnosticFailure(amp, DIAG_ASP_CONFIG, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_ASP);
    if (stopAt("asp"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_GPIO);
    if (!applyGPIO(amp)) {
        recordDiagnosticFailure(amp, DIAG_GPIO_CONFIG, cirrus::devices::amplifiers::cs35l41::registers::kRegGpioPadControl,
                                uint32_t(amp.gpio2Function) << 24);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_GPIO);
    if (stopAt("gpio"))
        return kIOReturnSuccess;

    setDiagnosticStage(amp, STAGE_PLATFORM);
    if (!configureHardware(amp)) {
        CIRRUS_ERR("platform hardware configuration failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_PLATFORM_CONFIG);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_PLATFORM);
    if (stopAt("platform"))
        return kIOReturnSuccess;

    return kIOReturnSuccess;
}

IOReturn CS35L41Driver::loadFirmwareFlow() {
    auto& amp = mAmplifier;
    const bool bypass = bootArgEnabled("-cirrusnodsp");

    if (bypass) {
        if (!stopDSP(amp)) {
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
    } else {
        setDiagnosticStage(amp, STAGE_FIRMWARE_DISCOVERY);
        discoverFirmware(amp);
        if (amp.firmwareIdentityPending) {
            amp.bringUpDisposition = cirrus::core::BringUpDisposition::Wait;
            return kIOReturnSuccess;
        }
        if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
            recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
            return kIOReturnNotReady;
        }
        markDiagnosticSuccess(amp, STAGE_FIRMWARE_DISCOVERY);
        setDiagnosticStage(amp, STAGE_FIRMWARE_UPLOAD);
        initializeFirmware(amp, "5D.0");
        if ((bootArgStrEquals("-cirrusphase", "firmware") && mHost.debugHalted()) && amp.firmwareValidated && !amp.playbackFaulted) {
            amp.bringUpDisposition = cirrus::core::BringUpDisposition::DebugHalt;
            return kIOReturnSuccess;
        }
        if (amp.playbackFaulted || !amp.firmwareValidated || !amp.dspAlive || amp.monitorCount != 1) {
            if (amp.diagnostic.latestFailure == DIAG_OK)
                recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
        markDiagnosticSuccess(amp, STAGE_DSP_BOOT);
    }

    if ((bypass && stopAt("firmware")) || stopAt("dsp")) {
        // Firmware initialization includes DSP boot; explicitly halt it before returning.
        if (!stopDSP(amp)) {
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            return kIOReturnNotReady;
        }
        return kIOReturnSuccess;
    }

    return kIOReturnSuccess;
}

IOReturn CS35L41Driver::verifyReadyFlow() {
    auto& amp = mAmplifier;
    const bool bypass = bootArgEnabled("-cirrusnodsp");

    setDiagnosticStage(amp, STAGE_IDLE_VERIFY);
    if (!powerUpAmplifier(amp)) {
        recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
        return kIOReturnNotReady;
    }
    if (!verifyIdleConfiguration(amp)) {
        CIRRUS_ERR("idle hardware verification failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_IDLE_INVARIANT);
        return kIOReturnNotReady;
    }
    if (!bypass && !verifyDSPAlive(amp)) {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        return kIOReturnNotReady;
    }
    markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
    logASPSnapshot(amp);

    amp.initialized = true;
    return kIOReturnSuccess;
}

IOReturn CS35L41Driver::shutdownFlow() {
    auto& amp = mAmplifier;

    bool clean = true;
    amp.initialized = amp.firmwareValidated = false;

    if (amp.present) {
        if (!stopPlayback(amp)) {
            clean = false;
            recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
        }
        if (!stopDSP(amp)) {
            clean = false;
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        }
    }
    amp.dspAlive = false;
    amp.monitorCount = 0;
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;
    return clean ? kIOReturnSuccess : kIOReturnNotReady;
}

bool CS35L41Driver::pollRegisterBit(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs, TraceSource source) {
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

    UInt32 st1 = 0, st2 = 0, st3 = 0, st4 = 0, pwr_ctrl2 = 0;
    readRegister(amp, 0x10010, &st1, source);
    readRegister(amp, 0x10014, &st2, source);
    readRegister(amp, 0x10018, &st3, source);
    readRegister(amp, 0x1001C, &st4, source);
    readRegister(amp, 0x02018, &pwr_ctrl2, source);
    CIRRUS_ERR("Amp %s: DIAGNOSTICS -> ST1=0x%08X ST2=0x%08X ST3=0x%08X ST4=0x%08X PWR_CTRL2=0x%08X", amp.name, st1, st2, st3, st4,
               pwr_ctrl2);

    return false;
}

CS35L41Driver::CS35L41Driver(const platform::AmplifierDriverContext& context)
    : mContext(context), mHost(*context.host), mRouteGuard{mHost} {
    auto& amp = mAmplifier;
    amp.name = mHost.name();
    amp.endpoint = context.endpoint;
    amp.address = uint8_t(context.endpoint.i2cAddress);
    amp.endpointIndex = mHost.endpointIndex();
    amp.registerIO = &context.io;
    amp.model = core::AmplifierModel::CS35L41;
    amp.channel = static_cast<core::AudioChannel>(context.board.amplifiers[context.bindingIndex].channelIndex);
    const auto& binding = context.board.amplifiers[context.bindingIndex];
    DeviceConfiguration configuration;
    if (decodeDeviceConfiguration(binding.driverConfiguration, binding.driverConfigurationSize, binding.driverConfigurationKind,
                                  configuration)) {
        amp.power = configuration.power;
        amp.gpio1Function = configuration.gpio1Function;
        amp.gpio2Function = configuration.gpio2Function;
        amp.explicitGPIOConfiguration =
            binding.driverConfigurationKind && !strcmp(binding.driverConfigurationKind, kDeviceConfigurationKind);
    }
    amp.deviceId = context.board.amplifiers[context.bindingIndex].siliconId;
    amp.firmwareSubsystemId = context.board.useBoardFirmwareIdentity
                                  ? (uint32_t(context.actualBoard.subsystemVendor) << 16 | context.actualBoard.subsystemDevice)
                                  : 0;
    amp.firmwareSpeakerId = binding.speakerId;
    amp.present = true;
    amp.diagnostic = mHost.diagnostics();
}

void CS35L41Driver::refreshRoute() {
    mHdaState = mHost.streamObservation();
    mAudioEvents = mHost.hasRouteEvents() ? &mRouteGuard : nullptr;
}

bool CS35L41Driver::synchronizeHdaStream() {
    const bool active = mHost.synchronizeRoute();
    refreshRoute();
    return active;
}

bool CS35L41Driver::stopAt(const char* phase) {
    if (!mHost.stopAfterDebugStage(phase))
        return false;
    mAmplifier.bringUpDisposition = core::BringUpDisposition::DebugHalt;
    return true;
}

IOReturn CS35L41Driver::identify() {
    uint32_t id = 0;
    const bool ok = readRegister(mAmplifier, registers::kRegDeviceId, &id, TRACE_PROBE);
    if (!ok || id != registers::kValDeviceId) {
        IOReturn result = ok ? kIOReturnNotFound : mLastTransferReturn;
        if (result == kIOReturnSuccess || result == -1)
            result = kIOReturnNotReady;
        mAmplifier.playbackFaulted = true;
        recordDiagnosticFailure(mAmplifier, DIAG_DEVICE_ID, registers::kRegDeviceId, registers::kValDeviceId, id, result);
        return result;
    }
    mAmplifier.deviceId = id;
    return kIOReturnSuccess;
}

IOReturn CS35L41Driver::authorizeBoard() {
    return mHost.authorized() && mContext.board.permits(mContext.actualBoard) &&
                   uint32_t(mAmplifier.channel) <= uint32_t(core::AudioChannel::Right)
               ? kIOReturnSuccess
               : kIOReturnNotPermitted;
}

IOReturn CS35L41Driver::initialize() {
    const auto r = initializeHardware();
    if (r != kIOReturnSuccess)
        mAmplifier.playbackFaulted = true;
    return r;
}

IOReturn CS35L41Driver::loadFirmware() {
    const auto r = loadFirmwareFlow();
    if (r != kIOReturnSuccess)
        mAmplifier.playbackFaulted = true;
    return r;
}

IOReturn CS35L41Driver::verifyReady() {
    const auto r = verifyReadyFlow();
    if (r != kIOReturnSuccess)
        mAmplifier.playbackFaulted = true;
    return r;
}

IOReturn CS35L41Driver::startPlayback() {
    refreshRoute();
    return commitCS35L41Playback(mAmplifier);
}

IOReturn CS35L41Driver::stopPlayback() {
    if (stopPlayback(mAmplifier))
        return kIOReturnSuccess;
    mAmplifier.playbackFaulted = true;
    return kIOReturnNotReady;
}

IOReturn CS35L41Driver::safeShutdown() {
    const auto r = shutdownFlow();
    if (r != kIOReturnSuccess)
        mAmplifier.playbackFaulted = true;
    return r;
}

void CS35L41Driver::captureDiagnostics() {
    refreshRoute();
    captureCS35L41FailureSnapshot(mAmplifier, mHost.diagnostics().latestFailure);
}

IOReturn CS35L41Driver::configureInterrupt(bool enabled) {
    if (mContext.board.amplifiers[mContext.bindingIndex].interruptGpio != 2)
        return kIOReturnUnsupported;
    return configureCS35L41Interrupt(mAmplifier, enabled) ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn CS35L41Driver::handleInterrupt() {
    if (checkProtectionStatus(mAmplifier))
        return kIOReturnSuccess;
    mAmplifier.playbackFaulted = true;
    return kIOReturnNotReady;
}

IOReturn CS35L41Driver::runtimeTick(const core::PlaybackContext& context) {
    refreshRoute();
    monitorCS35L41(mAmplifier, context);
    return kIOReturnSuccess;
}

bool CS35L41Driver::playbackNeedsPreparation() const {
    return mAmplifier.initialized && !mAmplifier.playbackActive &&
           (!mAmplifier.playbackFaulted || (mAmplifier.pllRecoveryPending && mAmplifier.pllRecoveryAttempts < 3));
}

bool CS35L41Driver::playbackPrepared() const {
    return mAmplifier.initialized && !mAmplifier.playbackFaulted && mAmplifier.playbackTransition.phase == PlaybackPhase::Prepared;
}

bool CS35L41Driver::playbackFaulted() const {
    return mAmplifier.playbackFaulted && (!mAmplifier.pllRecoveryPending || mAmplifier.pllRecoveryAttempts >= 3);
}

bool CS35L41Driver::validatePlayback(const core::PlaybackContext& context) {
    refreshRoute();
    return validateCS35L41Playback(mAmplifier, context);
}

IOReturn CS35L41Driver::cancelPlayback() {
    if (!mAmplifier.shutdownTransition.pending() && mAmplifier.cleanupAttempts < 3 &&
        (mAmplifier.playbackActive || mAmplifier.playbackTransition.pending()))
        stopRuntimePlayback(mAmplifier);
    return kIOReturnSuccess;
}

core::AmplifierReport CS35L41Driver::report() const {
    return {true,
            mAmplifier.initialized,
            playbackFaulted(),
            mAmplifier.firmwareIdentityPending,
            mAmplifier.present,
            mAmplifier.dspAlive && mAmplifier.firmwareValidated ? "READY_DSP"
            : mHost.bootArgEnabled("-cirrusnodsp")              ? "READY_BYPASS_EXPLICIT"
                                                                : "OUTPUT_DISABLED_DSP_UNVERIFIED",
            "WAITING_FOR_CODEC_IDENTITY"};
}

IOReturn CS35L41Driver::prepareBringUpRetry(uint64_t now) {
    if (!mAmplifier.firmwareIdentityPending)
        return mAmplifier.wmfwData && mAmplifier.binData ? kIOReturnSuccess : kIOReturnNotReady;
    discoverFirmware(mAmplifier);
    if (mAmplifier.firmwareIdentityPending) {
        if (now < mAmplifier.firmwareIdentityDeadlineMs)
            return kIOReturnNotReady;
        mAmplifier.firmwareIdentityPending = false;
        mAmplifier.playbackFaulted = true;
        recordDiagnosticFailure(mAmplifier, DIAG_FIRMWARE_MISSING, 0, 1, 0, kIOReturnTimeout);
        char prop[80];
        snprintf(prop, sizeof(prop), "Cirrus_Phase5A_Status_%s", mAmplifier.name);
        setProperty(prop, "CODEC_IDENTITY_TIMEOUT");
        return kIOReturnTimeout;
    }
    if (!mAmplifier.wmfwData || !mAmplifier.binData) {
        mAmplifier.playbackFaulted = true;
        recordDiagnosticFailure(mAmplifier, DIAG_FIRMWARE_MISSING);
        return kIOReturnNotFound;
    }
    return kIOReturnSuccess;
}

void CS35L41Driver::inspect() { probeAmplifier(mAmplifier); }

void CS35L41Driver::setDiagnosticStage(AmplifierState& amp, DriverStage stage) {
    mHost.setDiagnosticStage(stage);
    amp.diagnostic = mHost.diagnostics();
}

void CS35L41Driver::markDiagnosticSuccess(AmplifierState& amp, DriverStage stage) {
    mHost.markDiagnosticSuccess(stage);
    amp.diagnostic = mHost.diagnostics();
}

void CS35L41Driver::recordDiagnosticFailure(AmplifierState& amp, DiagnosticFailure failure, uint32_t reg, uint32_t expected,
                                            uint32_t actual, IOReturn result, bool capture) {
    mHost.recordDiagnosticFailure(failure, reg, expected, actual, result, capture);
    amp.diagnostic = mHost.diagnostics();
}

bool CS35L41Driver::readRegister(AmplifierState& amp, uint32_t reg, uint32_t* value, TraceSource source) {
    const bool ok = mContext.io.readWithTrace(reg, value, source);
    mLastTransferReturn = mContext.io.lastError();
    amp.diagnostic = mHost.diagnostics();
    return ok;
}

bool CS35L41Driver::writeRegister(AmplifierState& amp, uint32_t reg, uint32_t value, TraceSource source) {
    const bool ok = mContext.io.writeWithTrace(reg, value, source);
    mLastTransferReturn = mContext.io.lastError();
    amp.diagnostic = mHost.diagnostics();
    return ok;
}

bool CS35L41Driver::bulkRead(AmplifierState& amp, uint32_t reg, uint8_t* data, size_t n, TraceSource source) {
    const bool ok = mContext.io.bulkReadWithTrace(reg, data, n, source);
    mLastTransferReturn = mContext.io.lastError();
    amp.diagnostic = mHost.diagnostics();
    return ok;
}

bool CS35L41Driver::bulkWrite(AmplifierState& amp, uint32_t reg, const uint8_t* data, size_t n, TraceSource source) {
    const bool ok = mContext.io.bulkWriteWithTrace(reg, data, n, source);
    mLastTransferReturn = mContext.io.lastError();
    amp.diagnostic = mHost.diagnostics();
    return ok;
}

bool CS35L41Driver::updateRegisterBits(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t value, TraceSource source) {
    uint32_t old = 0;
    if (!readRegister(amp, reg, &old, source))
        return false;
    const uint32_t next = (old & ~mask) | (value & mask);
    return next == old || writeRegister(amp, reg, next, source);
}
}
