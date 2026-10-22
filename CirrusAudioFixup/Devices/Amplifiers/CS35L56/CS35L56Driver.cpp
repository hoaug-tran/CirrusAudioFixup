#include "Devices/Amplifiers/CS35L56/CS35L56Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
void CS35L56Driver::delayUs(uint32_t us) {
#if defined(KERNEL)
    if (us < 1000) {
        IODelay(us);
        return;
    }
#endif
    IOSleep(us / 1000 + (us % 1000 != 0));
}
void CS35L56Driver::publishState(const char* value) {
    char key[80]{};
    snprintf(key, sizeof(key), "Cirrus_CS35L56_%u_State", unsigned(mHost.endpointIndex()));
    mHost.publish(key, value);
}
IOReturn CS35L56Driver::fail(IOReturn result, diagnostics::DiagnosticFailure failure, uint32_t reg) {
    if (result == kIOReturnSuccess || result == -1)
        result = kIOReturnNotReady;
    if (mAmplifier.firstFailure == kIOReturnSuccess)
        mAmplifier.firstFailure = result;
    mAmplifier.latestFailure = result;
    mAmplifier.faulted = true;
    mAmplifier.routePrepared = false;
    mHost.recordDiagnosticFailure(failure, reg, 0, 0, result, false);
    publishState("FAILED_OUTPUT_DISABLED");
    return result;
}
IOReturn CS35L56Driver::transferFailure(diagnostics::DiagnosticFailure failure, uint32_t reg) {
    return fail(mContext.io.lastError(), failure, reg);
}
bool CS35L56Driver::writable() const { return mAmplifier.authorized && mHost.authorized() && !mHost.bootArgEnabled("-cirrusro"); }
bool CS35L56Driver::stopAt(const char* phase) {
    if (!mHost.stopAfterDebugStage(phase))
        return false;
    mAmplifier.bringUpDisposition = core::BringUpDisposition::DebugHalt;
    publishState("DEBUG_HALTED_OUTPUT_DISABLED");
    return true;
}
IOReturn CS35L56Driver::identify() {
    if (mAmplifier.hardwareTouched)
        return kIOReturnNotReady;
    mHost.setDiagnosticStage(diagnostics::STAGE_PROBE);
    probeAmplifier();
    if (mAmplifier.result != kIOReturnSuccess)
        return fail(mAmplifier.result, diagnostics::DIAG_DEVICE_ID, mAmplifier.failedRegister);
    if (!mAmplifier.revisionSupported)
        return fail(kIOReturnUnsupported, diagnostics::DIAG_DEVICE_ID, registers::kRegRevisionId);
    mHost.markDiagnosticSuccess(diagnostics::STAGE_PROBE);
    return kIOReturnSuccess;
}
IOReturn CS35L56Driver::authorizeBoard() {
    mAmplifier.authorized = false;
    if (!mAmplifier.identified || !mAmplifier.revisionSupported || !mHost.authorized() || mHost.bootArgEnabled("-cirrusro") ||
        !mContext.board.permits(mContext.actualBoard) || mContext.bindingIndex >= mContext.board.count || !mContext.actualDeviceHid ||
        strcmp(mContext.actualDeviceHid, "CSC3556"))
        return fail(kIOReturnNotPermitted, diagnostics::DIAG_PLATFORM_CONFIG);
    const auto& binding = mContext.board.amplifiers[mContext.bindingIndex];
    const firmware::FirmwareTarget expected{registers::kValDeviceId,
                                            mContext.actualBoard.subsystemVendor,
                                            mContext.actualBoard.subsystemDevice,
                                            binding.speakerId,
                                            binding.channelIndex,
                                            binding.tuningResource};
    if (binding.siliconId != registers::kValDeviceId || binding.channelIndex > 1 || !mContext.endpoint.sameDevice(binding.endpoint) ||
        mContext.endpoint.transport != core::BusTransportType::SPI || mContext.endpoint.spiChipSelect > 1 ||
        mContext.endpoint.spiMode != binding.endpoint.spiMode || mContext.endpoint.spiClockHz > binding.endpoint.spiClockHz ||
        !mContext.firmware.matches(expected) || mContext.firmware.format != firmware::FirmwareFormat::Deferred)
        return fail(kIOReturnNotPermitted, diagnostics::DIAG_PLATFORM_CONFIG);
    mResource = selectFirmwareResource(cs35l56Firmware, cs35l56FirmwareCount, expected.subsystemVendor, expected.subsystemDevice,
                                       expected.speakerId);
    size_t size = 0;
    if (!mResource || !mResource->wmfw || !mResource->wmfwSize || !selectChannelCoefficients(*mResource, binding.channelIndex == 1, size))
        return fail(kIOReturnNotFound, diagnostics::DIAG_FIRMWARE_MISSING);
    mAmplifier.authorized = true;
    return kIOReturnSuccess;
}
core::AmplifierReport CS35L56Driver::report() const {
    const char* verdict = !mAmplifier.firmwareValidated ? "OUTPUT_DISABLED_DSP_UNVERIFIED"
                          : mAmplifier.calibrated       ? "READY_DSP"
                                                        : "READY_DSP_OUTPUT_DISABLED_CALIBRATION";
    return {true, mAmplifier.initialized, mAmplifier.faulted, false, mAmplifier.identified, verdict, nullptr};
}
}
