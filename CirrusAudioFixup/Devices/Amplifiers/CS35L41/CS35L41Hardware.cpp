#include "Devices/Amplifiers/CS35L41/CS35L41Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
bool CS35L41Driver::initializeCS35L41Device(AmplifierState& amp) {
    setDiagnosticStage(amp, STAGE_PROBE);
    amp.present = false;

    DriverRegisterIO io(this, amp);
    auto device = amp.registerIO ? cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.endpoint)
                                 : cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.address);

    if (!device.probe(io)) {
        CIRRUS_ERR("failed to identify %s before reset", amp.name);
        return false;
    }

    amp.deviceId = device.deviceId();
    amp.revisionId = device.revisionId();
    amp.present = device.isPresent();

    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver; add -cirrusdbg and report hardware profile",
                   amp.deviceId, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::amplifiers::cs35l41::registers::kValDeviceId, amp.deviceId);
        return false;
    }

    bool deepDiag = bootArgEnabled("-cirrusdiag");
    UInt32 crcBefore = deepDiag ? calculateRegistersCRC32(amp) : 0;

    CIRRUS_LOG("amplifier %s status before reset: devid=0x%08X revid=0x%08X%s", amp.name, amp.deviceId, amp.revisionId,
               deepDiag ? " (deep diagnostics enabled)" : "");
    CIRRUS_LOG("sending soft reset to %s", amp.name);

    setDiagnosticStage(amp, STAGE_RESET);

    auto result = device.initialize(io);
    if (result == cirrus::core::AmplifierDevice::InitResult::ResetFailed) {
        CIRRUS_ERR("failed to send soft reset to %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_RESET_WRITE, cirrus::devices::amplifiers::cs35l41::registers::kRegSoftwareReset,
                                cirrus::devices::amplifiers::cs35l41::registers::kValSoftwareReset, 0, mLastTransferReturn);
        return false;
    }

    setDiagnosticStage(amp, STAGE_OTP_BOOT);
    if (result == cirrus::core::AmplifierDevice::InitResult::BootTimeout) {
        CIRRUS_ERR("otp boot complete polling failed on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_OTP_TIMEOUT, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Status4,
                                cirrus::devices::amplifiers::cs35l41::registers::kMaskOtpBootDone, 0);
        return false;
    }

    if (result == cirrus::core::AmplifierDevice::InitResult::BootError) {
        UInt32 otpStatus = 0;
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Status3, &otpStatus, TRACE_PROBE);
        recordDiagnosticFailure(amp, DIAG_OTP_BOOT_ERROR, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Status3, 0, otpStatus);
        return false;
    }

    if (result != cirrus::core::AmplifierDevice::InitResult::Success) {
        CIRRUS_ERR("failed to initialize %s", amp.name);
        return false;
    }

    UInt32 devIdAfter = 0, revIdAfter = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId, &devIdAfter) ||
        !readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegRevisionId, &revIdAfter)) {
        CIRRUS_ERR("failed to identify %s after reset", amp.name);
        return false;
    }

    amp.deviceId = devIdAfter;
    amp.revisionId = revIdAfter;
    amp.present = (devIdAfter == cirrus::devices::amplifiers::cs35l41::registers::kValDeviceId);
    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver after reset; add -cirrusdbg and report hardware profile",
                   devIdAfter, amp.name);
        recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId,
                                cirrus::devices::amplifiers::cs35l41::registers::kValDeviceId, devIdAfter);
        return false;
    }

    UInt32 crcAfter = deepDiag ? calculateRegistersCRC32(amp) : 0;

    char propName[64];
    if (deepDiag) {
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Before_%s", amp.name);
        setProperty(propName, crcBefore, 32);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_After_%s", amp.name);
        setProperty(propName, crcAfter, 32);
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X crc=0x%08X", amp.name, devIdAfter, revIdAfter, crcAfter);
    } else {
        CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X", amp.name, devIdAfter, revIdAfter);
    }

    UInt32 revOnly = revIdAfter & 0xFF;

    switch (revOnly) {
    case 0xB0:
    case 0xB1:
    case 0xB2:
        CIRRUS_LOG("revision 0x%02X verified for %s", revOnly, amp.name);
        break;
    default:
        CIRRUS_LOG("unknown chip revision 0x%02X found on %s", revOnly, amp.name);
        break;
    }

    return true;
}

bool CS35L41Driver::unlockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegTestKeyControl, 0x00000055);
    bool ret2 = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegTestKeyControl, 0x000000AA);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to unlock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys unlocked successfully on %s", amp.name);
    return true;
}

bool CS35L41Driver::lockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegTestKeyControl, 0x000000CC);
    bool ret2 = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegTestKeyControl, 0x00000033);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to lock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys locked successfully on %s", amp.name);
    return true;
}

bool CS35L41Driver::initializeHardwareErrata(AmplifierState& amp) {
    CIRRUS_LOG("starting hardware errata initialization for %s", amp.name);

    bool result = false;
    bool deepDiag = bootArgEnabled("-cirrusdiag");
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc);
    size_t allocSize = numRegs * sizeof(UInt32);

    char propName[64] = {0};
    UInt32 crcUnlock = 0;
    UInt32 crcErrata = 0;
    UInt32 crcOtp = 0;
    UInt32 crcLock = 0;

    UInt32* snapshot0 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot1 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot2 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;
    UInt32* snapshot3 = deepDiag ? (UInt32*)IOMallocData(allocSize) : nullptr;

    if (deepDiag && (!snapshot0 || !snapshot1 || !snapshot2 || !snapshot3)) {
        CIRRUS_ERR("failed to allocate memory for register snapshots on %s", amp.name);
        goto cleanup;
    }

    if (deepDiag)
        snapshotRegisters(amp, snapshot0);

    if (!unlockTestKey(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot1);
        crcUnlock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
        setProperty(propName, crcUnlock, 32);
        CIRRUS_LOG("checksum after unlock on %s: 0x%08X", amp.name, crcUnlock);
    }

    if (!applyErrataPatch(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot2);
        CIRRUS_LOG("register diffs after applying errata patch on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot1, snapshot2);
        crcErrata = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
        setProperty(propName, crcErrata, 32);
        CIRRUS_LOG("checksum after errata patch on %s: 0x%08X", amp.name, crcErrata);
    }

    if (!unpackOTP(amp)) {
        CIRRUS_ERR("otp unpacking failed on %s, locking test key as rollback", amp.name);
        recordDiagnosticFailure(amp, DIAG_OTP_UNPACK);
        lockTestKey(amp);
        goto cleanup;
    }

    if (deepDiag) {
        snapshotRegisters(amp, snapshot3);
        CIRRUS_LOG("register diffs after unpacking otp on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot2, snapshot3);
        crcOtp = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
        setProperty(propName, crcOtp, 32);
        CIRRUS_LOG("checksum after otp unpack on %s: 0x%08X", amp.name, crcOtp);
    }

    if (!lockTestKey(amp)) {
        goto cleanup;
    }

    if (deepDiag) {
        crcLock = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
        setProperty(propName, crcLock, 32);
        CIRRUS_LOG("checksum after locking test keys on %s: 0x%08X", amp.name, crcLock);
    }

    CIRRUS_LOG("hardware errata init completed for %s", amp.name);
    result = true;

cleanup:
    if (!result) {
        if (!lockTestKey(amp))
            amp.playbackFaulted = true;
    }
    if (snapshot0)
        IOFreeData(snapshot0, allocSize);
    if (snapshot1)
        IOFreeData(snapshot1, allocSize);
    if (snapshot2)
        IOFreeData(snapshot2, allocSize);
    if (snapshot3)
        IOFreeData(snapshot3, allocSize);

    return result;
}

bool CS35L41Driver::applyErrataPatch(AmplifierState& amp) {
    const ErrataTable errataTables[] = {{0xB2, cs35l41_revb2_errata_patch, sizeof(cs35l41_revb2_errata_patch) / sizeof(ErrataPatch)}};

    UInt32 revOnly = amp.revisionId & 0xFF;
    const ErrataTable* tableToApply = nullptr;

    for (size_t i = 0; i < sizeof(errataTables) / sizeof(ErrataTable); i++) {
        if (errataTables[i].revid == revOnly) {
            tableToApply = &errataTables[i];
            break;
        }
    }

    if (!tableToApply) {
        CIRRUS_ERR("no errata patches found for revision 0x%02X on %s", revOnly, amp.name);
        return false;
    }

    CIRRUS_LOG("applying %lu errata patches for revision 0x%02X on %s", tableToApply->numPatches, revOnly, amp.name);

    for (size_t i = 0; i < tableToApply->numPatches; i++) {
        if (!writeRegister(amp, tableToApply->patches[i].reg, tableToApply->patches[i].value)) {
            CIRRUS_ERR("failed to apply errata patch at register 0x%08X on %s", tableToApply->patches[i].reg, amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA, tableToApply->patches[i].reg, tableToApply->patches[i].value, 0, mLastTransferReturn);
            return false;
        }
    }

    if (!updateRegisterBits(amp, 0x02BC1000, cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable, 0x00000000)) {
        CIRRUS_ERR("failed to disable dsp core in core ctrl on %s", amp.name);
        return false;
    }

    return true;
}

bool CS35L41Driver::unpackOTP(AmplifierState& amp) {
    DriverRegisterIO io(this, amp);
    auto device = amp.registerIO ? cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.endpoint)
                                 : cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.address);
    if (!device.unpackOTP(io)) {
        CIRRUS_ERR("failed to unpack OTP on %s", amp.name);
        return false;
    }
    CIRRUS_LOG("otp unpacking complete on %s", amp.name);
    return true;
}

bool CS35L41Driver::applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sequence[i].updateBits) {
            if (!updateRegisterBits(amp, sequence[i].reg, sequence[i].mask, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        } else {
            if (!writeRegister(amp, sequence[i].reg, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        }
        if (sequence[i].delayUs > 0) {
            IODelay(sequence[i].delayUs);
        }
    }
    return true;
}

bool CS35L41Driver::applyPLL(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    DriverRegisterIO io(this, amp);
    auto device = amp.registerIO ? cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.endpoint)
                                 : cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.address);

    bool pllLocked = false;
    if (!device.setupPLL(io, &pllLocked)) {
        return false;
    }

    if (!pllLocked) {
        CIRRUS_LOG("warning: PLL did NOT lock on %s. I2S clocks might be inactive.", amp.name);
    } else {
        CIRRUS_LOG("PLL lock verified successfully on %s", amp.name);
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (amp.channel == cirrus::core::AudioChannel::Right) {
        setProperty("Cirrus_PLL_CRC_right", crcAfter, 32);
        setProperty("Cirrus_PLL_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_PLL_CRC_left", crcAfter, 32);
        setProperty("Cirrus_PLL_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

bool CS35L41Driver::applyASP(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    DriverRegisterIO io(this, amp);
    auto device = amp.registerIO ? cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.endpoint)
                                 : cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.address);

    if (!device.setupASP(io, amp.channel == cirrus::core::AudioChannel::Right, amp.firmwareValidated)) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_ASP_CRC_right", crcAfter, 32);
        setProperty("Cirrus_ASP_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_ASP_CRC_left", crcAfter, 32);
        setProperty("Cirrus_ASP_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

bool CS35L41Driver::applyGPIO(AmplifierState& amp) {
    uint64_t startTime = mach_absolute_time();

    DriverRegisterIO io(this, amp);
    auto device = amp.registerIO ? cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.endpoint)
                                 : cirrus::devices::amplifiers::cs35l41::CS35L41Device(amp.name, amp.address);

    if (!device.setupGPIO(io, amp.gpio1Function, amp.gpio2Function, amp.explicitGPIOConfiguration)) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsedUs = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsedUs);
    elapsedUs /= 1000;

    uint32_t crcAfter = bootArgEnabled("-cirrusdiag") ? calculateRegistersCRC32(amp) : 0;

    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_GPIO_CRC_right", crcAfter, 32);
        setProperty("Cirrus_GPIO_TimeUS_right", elapsedUs, 32);
    } else {
        setProperty("Cirrus_GPIO_CRC_left", crcAfter, 32);
        setProperty("Cirrus_GPIO_TimeUS_left", elapsedUs, 32);
    }

    return true;
}

bool CS35L41Driver::configureHardware(AmplifierState& amp) {
    CIRRUS_LOG("configuring generic serial port settings on %s", amp.name);
    bool ok = true;

    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1Mask4, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq2Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq2Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq2Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq2Mask4, 0xFFFFFFFF) && ok;

    ok = writeRegister(amp, 0x00002020, 0x00000006) && ok;

    DriverRegisterIO io(this, amp);
    ok = amp.power.configure(io) && ok;
    if (ok)
        CIRRUS_LOG("generic serial port config complete on %s", amp.name);
    return ok;
}

bool CS35L41Driver::powerUpAmplifier(AmplifierState& amp) {
    CIRRUS_LOG("starting power up sequence for %s (DSP Mode=%d)", amp.name, amp.monitorCount);
    bool ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678);
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, 0) && ok;
    if (!ok || !checkProtectionStatus(amp)) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
        return false;
    }

    if (amp.monitorCount == 1) {
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortEnables, 0x00010001) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000003) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDacPcm1Source, 0x00000032) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAspTx3Source, 0x00000028) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAspTx4Source, 0x00000029) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx2Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx5Source, amp.power.dspSupplyMonitorSource()) &&
             ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx6Source, 0x00000029) && ok;
    } else {
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortEnables, 0x00010000) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000002) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDacPcm1Source, 0x08) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAspTx3Source, 0) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAspTx4Source, 0) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx2Source, 0x00000009) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx5Source, 0x00000020) && ok;
        ok = writeRegister(amp, registers::kRegDsp1Rx6Source, 0x21) && ok;
    }

    ok = updateRegisterBits(amp, 0x00002018, 0x00000001, 0x00000000) && ok;

    if (!amp.power.internal()) {
        ok = writeRegister(amp, 0x00000040, 0x00000055) && ok;
        ok = writeRegister(amp, 0x00000040, 0x000000AA) && ok;
    }
    ok = updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegPowerControl1, 1, 0) && ok;
    if (!amp.power.internal()) {
        ok = writeRegister(amp, 0x00007438, 0x00585941) && ok;
        ok = writeRegister(amp, 0x00007414, 0x08C82222) && ok;
        ok = writeRegister(amp, 0x0000742C, 0x00000009) && ok;
    }
    ok = updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegPowerControl2,
                            0x00003000 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable, 0) &&
         ok;
    if (!amp.power.internal()) {
        ok = writeRegister(amp, 0x00000040, 0x000000CC) && ok;
        ok = writeRegister(amp, 0x00000040, 0x00000033) && ok;
    }

    uint32_t digVol = 0, gainCtrl = 0;
    ok = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol) && ok;
    ok = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, &gainCtrl) && ok;
    CIRRUS_LOG("default volume states on %s: digital_volume=0x%08X gain=0x%08X", amp.name, digVol, gainCtrl);
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678) && ok;
    ok = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, 0x00000000) && ok;

    if (!ok) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
    }
    logPowerSnapshot(amp);
    snapshotDiagnostics(amp, "IDLE (POST-BOOT)");
    return ok;
}

bool CS35L41Driver::verifyIdleConfiguration(AmplifierState& amp) {
    uint32_t pwr1 = 0, pwr2 = 0, spEn = 0, spFmt = 0;
    uint32_t dacSrc = 0, digVol = 0, gain = 0;
    bool readable =
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegPowerControl1, &pwr1, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegPowerControl2, &pwr2, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortEnables, &spEn, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFormat, &spFmt, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDacPcm1Source, &dacSrc, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol, TRACE_PROBE) &&
        readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PROBE);
    bool dspMode = amp.monitorCount >= 1;
    uint32_t expectedSpEn = dspMode ? 0x00010001U : 0x00010000U;
    uint32_t expectedDac = dspMode ? 0x00000032U : 0x00000008U;
    bool valid = readable && ((pwr1 & 0x1) == 0) &&
                 ((pwr2 & (0x00003001 | cirrus::devices::amplifiers::cs35l41::registers::kMaskBoostEnable)) == 0) && spEn == expectedSpEn &&
                 spFmt == 0x20200200U && dacSrc == expectedDac && digVol == 0x0000A678U && gain == 0;
    if (!valid) {
        CIRRUS_ERR("idle verify %s: read=%d pwr1=0x%08X pwr2=0x%08X sp_en=0x%08X sp_fmt=0x%08X dac=0x%08X vol=0x%08X gain=0x%08X mode=%s",
                   amp.name, readable, pwr1, pwr2, spEn, spFmt, dacSrc, digVol, gain, dspMode ? "DSP" : "BYPASS");
    }
    return valid;
}
}
