#include "Devices/Amplifiers/CS35L56/CS35L56Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
IOReturn CS35L56Driver::initialize() {
    using namespace registers;
    if (!writable() || mAmplifier.faulted || !mResource || mAmplifier.hardwareTouched)
        return kIOReturnNotPermitted;
    if (stopAt("probe"))
        return kIOReturnSuccess;
    mHost.setDiagnosticStage(diagnostics::STAGE_OTP_BOOT);
    // Linux auto-wake: the first read can fail while the control port wakes.
    uint32_t ignored = 0;
    mContext.io.read(kRegIrqStatus, &ignored);
    delayUs(kWakeHoldTimeUs);
    mContext.io.read(kRegIrqStatus, &ignored);
    delayUs(kControlPortReadyUs);
    if (!DSPControl::waitForBoot(mContext.io, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegDspHaloState);
    mAmplifier.booted = true;
    uint32_t restricted = 0, protection = 0;
    if (!mContext.io.read(kRegRestricted, &restricted) || !mContext.io.read(kRegProtectionStatus, &protection) ||
        !mContext.io.read(kRegFirmwareVersion, &mAmplifier.firmwareVersion))
        return transferFailure(diagnostics::DIAG_DSP_BOOT);
    if (restricted & kRestrictedMask)
        return fail(kIOReturnUnsupported, diagnostics::DIAG_DSP_BOOT, kRegRestricted);
    mAmplifier.firmwareMissing = (protection & kFirmwareMissingMask) != 0;
    if (!mAmplifier.firmwareMissing && mAmplifier.firmwareVersion != kFirmwareVersion)
        return fail(kIOReturnUnsupported, diagnostics::DIAG_DSP_BOOT, kRegFirmwareVersion);
    // From here cleanup must be attempted even if the first write fails.
    mAmplifier.hardwareTouched = true;
    // Linux HDA performs an initial system reset before configuring or patching
    // either ROM or a compatible BIOS-loaded image.
    mAmplifier.booted = false;
    if (!DSPControl::resetSPI(mContext.io, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegDspVirtualMailbox2);
    mAmplifier.booted = true;
    if (!DSPControl::sendMailbox(mContext.io, kCommandPreventHibernate, delayUs) || !mContext.io.read(kRegProtectionStatus, &protection) ||
        !mContext.io.read(kRegFirmwareVersion, &mAmplifier.firmwareVersion))
        return transferFailure(diagnostics::DIAG_DSP_BOOT);
    mAmplifier.firmwareMissing = (protection & kFirmwareMissingMask) != 0;
    if (!mAmplifier.firmwareMissing && mAmplifier.firmwareVersion != kFirmwareVersion)
        return fail(kIOReturnUnsupported, diagnostics::DIAG_DSP_BOOT, kRegFirmwareVersion);
    if (!mContext.io.write(kRegAspEnables, 0) || !mContext.io.write(kRegUserMute, 1) || !mContext.io.write(kRegIrqMask20, 0xFFFFFFFF))
        return transferFailure(diagnostics::DIAG_PLATFORM_CONFIG);
    mHost.markDiagnosticSuccess(diagnostics::STAGE_OTP_BOOT);
    return kIOReturnSuccess;
}
IOReturn CS35L56Driver::verifyReady() {
    using namespace registers;
    if (!writable() || mAmplifier.faulted || !mAmplifier.firmwareValidated || !mAmplifier.booted)
        return kIOReturnNotReady;
    mHost.setDiagnosticStage(diagnostics::STAGE_IDLE_VERIFY);
    uint32_t enables = 0, mute = 0, version = 0, protection = 0;
    if (!DSPControl::waitForBoot(mContext.io, delayUs) || !mContext.io.read(kRegAspEnables, &enables) ||
        !mContext.io.read(kRegUserMute, &mute) || !mContext.io.read(kRegFirmwareVersion, &version) ||
        !mContext.io.read(kRegProtectionStatus, &protection))
        return transferFailure(diagnostics::DIAG_IDLE_ROLLBACK);
    if (enables || mute != 1 || version != kFirmwareVersion || (protection & kFirmwareMissingMask))
        return fail(kIOReturnNotReady, diagnostics::DIAG_IDLE_ROLLBACK);
    mAmplifier.firmwareVersion = version;
    char key[80]{};
    snprintf(key, sizeof(key), "Cirrus_CS35L56_%u_Firmware_Version", unsigned(mHost.endpointIndex()));
    mHost.publish(key, version, 32);
    snprintf(key, sizeof(key), "Cirrus_CS35L56_%u_Calibration", unsigned(mHost.endpointIndex()));
    mHost.publish(key, mAmplifier.calibrated ? "APPLIED_AND_VERIFIED" : "UNAVAILABLE_OUTPUT_DISABLED");
    mAmplifier.initialized = true;
    mHost.markDiagnosticSuccess(diagnostics::STAGE_IDLE_VERIFY);
    publishState(mAmplifier.calibrated ? "READY_DSP_MUTED" : "READY_DSP_CALIBRATION_REQUIRED");
    stopAt("dsp");
    return kIOReturnSuccess;
}
IOReturn CS35L56Driver::safeShutdown() {
    mAmplifier.routePrepared = false;
    if (!mAmplifier.hardwareTouched)
        return kIOReturnSuccess;
    if (!writable())
        return kIOReturnNotPermitted;
    auto result = stopPlayback();
    if (result == kIOReturnSuccess && mAmplifier.firmwareValidated && !DSPControl::waitForIdle(mContext.io, delayUs))
        result = transferFailure(diagnostics::DIAG_IDLE_ROLLBACK, registers::kRegTransducerState);
    mAmplifier.initialized = mAmplifier.firmwareValidated = mAmplifier.calibrated = false;
    if (result == kIOReturnSuccess) {
        mAmplifier.hardwareTouched = false;
        mAmplifier.booted = false;
        // Reinitialization reads live firmware state after sleep; no stale replay.
        mAmplifier.faulted = false;
        mAmplifier.bringUpDisposition = core::BringUpDisposition::Continue;
    }
    return result;
}
}
