#include "Devices/Amplifiers/CS35L41/CS35L41Internal.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
void CS35L41Driver::probeAmplifier(AmplifierState& amp) {
    if (amp.model != cirrus::core::AmplifierModel::CS35L41) {
        CIRRUS_LOG("endpoint %s model=%u cached ID=0x%08X; diagnostic registers belong to its driver", amp.name, unsigned(amp.model),
                   amp.deviceId);
        return;
    }
    UInt32 deviceId = 0;
    UInt32 revisionId = 0;

    if (amp.registerIO && amp.endpoint.transport == cirrus::core::BusTransportType::SPI)
        CIRRUS_LOG("amp %s probe SPI controller=%s CS=%u", amp.name, amp.endpoint.controllerPath, amp.endpoint.spiChipSelect);
    else
        CIRRUS_LOG("amp %s probe address=0x%02X", amp.name, amp.address);

    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId, &deviceId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s device-id read failed", amp.name);
        return;
    }

    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRevisionIdRegister, &revisionId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s revision read failed", amp.name);
        return;
    }

    amp.deviceId = deviceId;
    amp.revisionId = revisionId;
    amp.present = (deviceId == cirrus::devices::amplifiers::cs35l41::registers::kValDeviceId);
    if (!amp.present) {
        CIRRUS_ERR("Amplifier device-id 0x%08X on %s is not handled by this driver; add -cirrusdbg and report hardware profile", deviceId,
                   amp.name);
    }

    if (amp.present) {
        dumpAllRegisters(amp);
    }

    CIRRUS_LOG("amp %s devid=0x%08X revision=0x%08X present=%s", amp.name, amp.deviceId, amp.revisionId, amp.present ? "yes" : "no");
}

void CS35L41Driver::captureCS35L41FailureSnapshot(AmplifierState& amp, DiagnosticFailure failure) {
    if (mCapturingFailureSnapshot)
        return;
    mCapturingFailureSnapshot = true;
    mHost.beginDiagnosticCapture();
    uint32_t devid = 0, revid = 0, pwr1 = 0, pwr2 = 0, pwr3 = 0, pwrSts = 0;
    uint32_t irq1 = 0, irq2 = 0, irq3 = 0, irq4 = 0, pll = 0;
    uint32_t spEn = 0, spRate = 0, spFmt = 0, spHiz = 0, rxSlot = 0, dac = 0;
    uint32_t core = 0, halo = 0, mbox1 = 0, mbox2 = 0, scratch1 = 0;
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDeviceId, &devid, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRevisionIdRegister, &revid, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl1, &pwr1, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl2, &pwr2, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerControl3, &pwr3, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kPowerManagementStatus, &pwrSts, TRACE_DUMP);
    readRegister(amp, 0x00010010, &irq1, TRACE_DUMP);
    readRegister(amp, 0x00010014, &irq2, TRACE_DUMP);
    readRegister(amp, 0x00010018, &irq3, TRACE_DUMP);
    readRegister(amp, 0x0001001C, &irq4, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1RawStatus3, &pll, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortEnables, &spEn, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortRateControl, &spRate, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFormat, &spFmt, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortHighImpedanceControl, &spHiz, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFrameRxSlot, &rxSlot, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDacPcm1Source, &dac, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &core, TRACE_DUMP);
    bool haloReadable = amp.haloStateRegister && readRegister(amp, amp.haloStateRegister, &halo, TRACE_DUMP);
    CIRRUS_LOG("HALO snapshot on %s: address=0x%08X readable=%d", amp.name, amp.haloStateRegister, haloReadable);
    readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kDspMbox2Register, &mbox2, TRACE_DUMP);
    readRegister(amp, 0x02B805C0, &scratch1, TRACE_DUMP);
    mCapturingFailureSnapshot = false;
    mHost.endDiagnosticCapture();

    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s code=%s hda_observed=%d hda_run=%d sd=%u tag=%u fmt=0x%04X", amp.name, failureName(failure),
               mHdaState.observed, mHdaState.streamActive, mHdaState.lastDescriptor, mHdaState.lastStreamTag, mHdaState.lastFormat);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s id=0x%08X rev=0x%08X pwr=[0x%08X 0x%08X 0x%08X] pwr_sts=0x%08X", amp.name, devid, revid, pwr1, pwr2,
               pwr3, pwrSts);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s irq=[0x%08X 0x%08X 0x%08X 0x%08X] pll_lock=%d asp=[en=0x%08X rate=0x%08X fmt=0x%08X hiz=0x%08X "
               "slot=0x%08X dac=0x%08X]",
               amp.name, irq1, irq2, irq3, irq4, (pll & 0x2) != 0, spEn, spRate, spFmt, spHiz, rxSlot, dac);
    CIRRUS_ERR("DIAG_SNAPSHOT amp=%s dsp=[core=0x%08X halo=0x%08X mbox1=0x%08X mbox2=0x%08X scratch1=0x%08X]", amp.name, core, halo, mbox1,
               mbox2, scratch1);
}

uint32_t CS35L41Driver::calculateRegistersCRC32(AmplifierState& amp) {
    uint32_t crc = 0;
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable)
            continue;
        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            crc = crc32Le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32Le(crc, (const uint8_t*)&val, 4);
        }
    }
    return crc;
}

void CS35L41Driver::dumpAllRegisters(AmplifierState& amp) {
    // Read-only and probe modes publish snapshots without routine verbosity.
    // Normal Release discovery must not perform an unsolicited register sweep.
    if (!gCirrusDebug && !bootArgEnabled("-cirrusro") && !bootArgStrEquals("-cirrusphase", "probe"))
        return;
    bool compact = bootArgEnabled("-cirruscompact");
    CIRRUS_LOG("starting full register dump for %s", amp.name);

    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    uint32_t successCount = 0;
    uint32_t crc = 0;
    uint32_t failedCount = 0, consecutiveFailures = 0, firstFailedRegister = 0;
    IOReturn firstFailure = kIOReturnSuccess;
    const uint64_t started = playbackTimeMilliseconds();
    const char* status = "COMPLETE";

    size_t bufferSize = 16 * 1024;
    char* dumpBuffer = (char*)IOMallocData(bufferSize);
    if (!dumpBuffer)
        return;
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;

    char lineBuffer[128];

    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable)
            continue;
        if (playbackTimeMilliseconds() - started >= 250) {
            status = "PARTIAL_TIME_BUDGET";
            break;
        }

        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            consecutiveFailures = 0;
            successCount++;
            crc = crc32Le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32Le(crc, (const uint8_t*)&val, 4);

            if (compact) {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X: %08X\n", cs35l41_reg_desc[i].addr, val);
            } else {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X  %-35s = %08X\n", cs35l41_reg_desc[i].addr, cs35l41_reg_desc[i].name, val);
            }

            size_t lineLen = strlen(lineBuffer);
            if (currentLen + lineLen < bufferSize - 1) {
                strlcat(dumpBuffer, lineBuffer, bufferSize);
                currentLen += lineLen;
            }
        } else {
            if (!failedCount) {
                firstFailedRegister = cs35l41_reg_desc[i].addr;
                firstFailure = mLastTransferReturn;
            }
            ++failedCount;
            if (++consecutiveFailures >= 3) {
                status = "PARTIAL_BUS_FAILURE";
                break;
            }
        }
    }
    if (failedCount && !strcmp(status, "COMPLETE"))
        status = "PARTIAL_READ_ERRORS";
    char statusProperty[80];
    snprintf(statusProperty, sizeof(statusProperty), "Cirrus_Dump_Status_%s", amp.name);
    OSString* dumpStatus = OSString::withCString(status);
    if (dumpStatus) {
        setProperty(statusProperty, dumpStatus);
        dumpStatus->release();
    }
    CIRRUS_LOG("BOOT_FLOW CS35L41 DUMP_END amp=%s status=%s reads=%u failures=%u first_reg=0x%08X first_ret=0x%08X", amp.name, status,
               successCount, failedCount, firstFailedRegister, uint32_t(firstFailure));
    CIRRUS_LOG("dump complete: %u registers successfully read, crc32: 0x%08X", successCount, crc);

    snprintf(lineBuffer, sizeof(lineBuffer), "status: %s; failed: %u; first: 0x%08X/0x%08X; crc32: 0x%08X\n", status, failedCount,
             firstFailedRegister, uint32_t(firstFailure), crc);
    strlcat(dumpBuffer, lineBuffer, bufferSize);

    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_Dump_%s", amp.name);

    OSString* strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propName, strObj);
        strObj->release();
    }
    IOFreeData(dumpBuffer, bufferSize);
}

void CS35L41Driver::runTimeBasedFSMCheck(AmplifierState& amp) {
    CIRRUS_LOG("starting finite state machine check for %s", amp.name);

    uint32_t crcT0 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t0 checksum for %s: 0x%08X", amp.name, crcT0);

    IOSleep(10);
    uint32_t crcT1 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t1 checksum for %s: 0x%08X", amp.name, crcT1);

    IOSleep(20);
    uint32_t crcT5 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t5 checksum for %s: 0x%08X", amp.name, crcT5);

    IOSleep(50);
    uint32_t crcT30 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t30 checksum for %s: 0x%08X", amp.name, crcT30);

    if (crcT0 == crcT1 && crcT1 == crcT5 && crcT5 == crcT30) {
        CIRRUS_LOG("fsm state is stable on %s", amp.name);
    } else {
        CIRRUS_LOG("fsm state is changing on %s", amp.name);
    }
}

void CS35L41Driver::snapshotRegisters(AmplifierState& amp, UInt32* snapshot) {
    if (!amp.present)
        return;
    for (size_t i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
        UInt32 val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val)) {
            snapshot[i] = val;
        } else {
            snapshot[i] = 0xFFFFFFFF;
        }
    }
}

void CS35L41Driver::compareRegisterSnapshots(AmplifierState& amp, const UInt32* oldSnapshot, const UInt32* newSnapshot) {
    if (!amp.present)
        return;
    CIRRUS_LOG("comparing register diffs on %s:", amp.name);
    int diffCount = 0;

    for (size_t i = 0; i < sizeof(cs35l41_reg_desc) / sizeof(RegisterDesc); i++) {
        if (oldSnapshot[i] != newSnapshot[i] && oldSnapshot[i] != 0xFFFFFFFF && newSnapshot[i] != 0xFFFFFFFF) {
            CIRRUS_LOG("Amp %s: [DIFF] %s (0x%08X) changed from 0x%08X to 0x%08X", amp.name, cs35l41_reg_desc[i].name,
                       cs35l41_reg_desc[i].addr, oldSnapshot[i], newSnapshot[i]);
            diffCount++;
        }
    }

    CIRRUS_LOG("Amp %s: Total %d registers changed.", amp.name, diffCount);
}

void CS35L41Driver::logASPSnapshot(AmplifierState& amp) {
    uint32_t enables = 0, rate = 0, fmt = 0, hiz = 0;
    uint32_t txWordLength = 0, rxWordLength = 0, txSlot = 0, rxSlot = 0;
    uint32_t rx1Src = 0, rx2Src = 0, rx3Src = 0, rx4Src = 0, rx5Src = 0;

    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortEnables, &enables);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortRateControl, &rate);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFormat, &fmt);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortHighImpedanceControl, &hiz);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFrameTxSlot, &txSlot);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortFrameRxSlot, &rxSlot);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortTxWordLength, &txWordLength);
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegSerialPortRxWordLength, &rxWordLength);
    readRegister(amp, 0x00004C40, &rx1Src);
    readRegister(amp, 0x00004C44, &rx2Src);
    readRegister(amp, 0x00004C48, &rx3Src);
    readRegister(amp, 0x00004C4C, &rx4Src);
    readRegister(amp, 0x00004C50, &rx5Src);

    CIRRUS_LOG("asp snapshot for %s: enables=0x%08X rate=0x%08X format=0x%08X hiz=0x%08X", amp.name, enables, rate, fmt, hiz);
    CIRRUS_LOG("asp snapshot slots for %s: rx_slot=0x%08X tx_slot=0x%08X rx_wl=0x%08X tx_wl=0x%08X", amp.name, rxSlot, txSlot, rxWordLength,
               txWordLength);
    CIRRUS_LOG("asp snapshot routing for %s: rx1_src=0x%08X rx2_src=0x%08X rx3_src=0x%08X rx4_src=0x%08X rx5_src=0x%08X", amp.name, rx1Src,
               rx2Src, rx3Src, rx4Src, rx5Src);

    bool pass = true;
    uint32_t expectedRxSlot = (strcmp(amp.name, "right") == 0) ? 1 : 0;
    if ((rxSlot & 0x3F) != expectedRxSlot) {
        CIRRUS_ERR("asp rx slot mismatch on %s", amp.name);
        pass = false;
    }

    uint32_t expectedRx1Src = 0x08;
    uint32_t expectedRx2Src = (amp.monitorCount >= 1) ? 0x08 : 0x09;
    if (rx1Src != expectedRx1Src) {
        CIRRUS_ERR("asp rx1 source routing mismatch on %s", amp.name);
        pass = false;
    }
    if (rx2Src != expectedRx2Src) {
        CIRRUS_ERR("asp rx2 source routing mismatch on %s", amp.name);
        pass = false;
    }

    if (pass) {
        CIRRUS_LOG("asp validation check: pass on %s", amp.name);
    } else {
        CIRRUS_ERR("asp validation check: fail on %s", amp.name);
    }
}

void CS35L41Driver::logDSPSnapshot(AmplifierState& amp) {
    uint32_t dspState = 0, mbox1 = 0, mbox2 = 0;
    readRegister(amp, 0x00013004, &mbox2);
    readRegister(amp, 0x00013020, &mbox1);
    readRegister(amp, 0x02BC1000, &dspState);

    CIRRUS_LOG("dsp status snapshot for %s: state=0x%08X core=0x%08X mbox1=%u mbox2=%u", amp.name, mbox2, dspState, mbox1, mbox2);
}

void CS35L41Driver::logDSPBootReport(AmplifierState& amp) {
    uint32_t coreCtrl = 0, sysId = 0, haloState = 0;
    uint32_t mbox1 = 0, mbox2 = 0;
    uint32_t xmVio = 0, ymVio = 0, pmVio = 0;
    uint32_t xmVioAddr = 0, ymVioAddr = 0, pmVioAddr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    uint32_t ts0 = 0, ts1 = 0, ts2 = 0;

    bool readable = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &coreCtrl, TRACE_DUMP);
    readable = readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemId, &sysId, TRACE_DUMP) && readable;
    readable = amp.haloStateRegister && readRegister(amp, amp.haloStateRegister, &haloState, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3104, &xmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ymVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ymVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pmVioAddr, TRACE_DUMP) && readable;
    readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP);
    readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP);
    readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP);
    readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP);

    bool hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts0, TRACE_DUMP);
    IODelay(2000);
    hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts1, TRACE_DUMP) && hbReadable;
    IODelay(2000);
    hbReadable = amp.haloHeartbeatRegister && readRegister(amp, amp.haloHeartbeatRegister, &ts2, TRACE_DUMP) && hbReadable;

    bool coreEn = (coreCtrl & cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable) != 0;
    bool coreOutOfReset = (coreCtrl & cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset) == 0;
    bool sysIdOk = (sysId != 0x00000000 && sysId != 0xFFFFFFFF);
    bool runOk = (haloState == 2);
    bool mboxOk = (mbox2 == cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxRunning ||
                   mbox2 == cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxPaused);
    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpuClean = ((xmVio & MPU_VIO_MASK) == 0 && (ymVio & MPU_VIO_MASK) == 0 && (pmVio & MPU_VIO_MASK) == 0);
    bool hbAlive = (ts0 != ts1 && ts1 != ts2);

    const char* verdict;
    const char* rootCause;
    if (!readable) {
        verdict = "UNVERIFIED";
        rootCause = "required register read failed or control unresolved";
    } else if (!amp.firmwareValidated || !amp.dspAlive) {
        verdict = "DISABLED";
        rootCause = "initialization failed; current registers may reflect cleanup";
    } else if (coreEn && coreOutOfReset && sysIdOk && runOk && mpuClean && mboxOk) {
        verdict = "HEALTHY";
        rootCause = "HALO RUN and mailbox verified; heartbeat is diagnostic only";
    } else {
        verdict = "FAIL";
        rootCause = "required DSP state invariant failed; inspect raw registers";
    }

    CIRRUS_LOG("===== DSP BOOT REPORT for %s =====", amp.name);
    CIRRUS_LOG("  VERDICT   : %s", verdict);
    CIRRUS_LOG("  ROOT CAUSE: %s", rootCause);
    CIRRUS_LOG("  core      : ctrl=0x%08X en=%s reset_released=%s", coreCtrl, coreEn ? "yes" : "NO", coreOutOfReset ? "yes" : "NO");
    CIRRUS_LOG("  sys_id    : 0x%08X (%s)", sysId, sysIdOk ? "ok" : "BAD");
    CIRRUS_LOG("  halo_state: 0x%08X (%s, want RUN=2)", haloState, runOk ? "RUN" : "not-run");
    CIRRUS_LOG("  mailbox   : mbox1=0x%08X mbox2=0x%08X (%s)", mbox1, mbox2, mboxOk ? "ok" : "not-running");
    CIRRUS_LOG("  mpu       : %s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X pm_vio=0x%08X@0x%08X", mpuClean ? "clean" : "FAULT", xmVio,
               xmVioAddr, ymVio, ymVioAddr, pmVio, pmVioAddr);
    CIRRUS_LOG("  scratch   : [0x%08X 0x%08X 0x%08X 0x%08X] (firmware panic code if non-zero)", sc1, sc2, sc3, sc4);
    CIRRUS_LOG("  heartbeat : t0=0x%08X t1=0x%08X t2=0x%08X (%s)", ts0, ts1, ts2,
               !hbReadable ? "READ_FAILED"
               : hbAlive   ? "CHANGING"
                           : "UNCHANGED_NOT_A_BOOT_VERDICT");
    CIRRUS_LOG("===== END DSP BOOT REPORT for %s =====", amp.name);

    char propName[80];
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_VERDICT_%s", amp.name);
    OSString* vStr = OSString::withCString(verdict);
    if (vStr) {
        setProperty(propName, vStr);
        vStr->release();
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_ROOTCAUSE_%s", amp.name);
    OSString* rStr = OSString::withCString(rootCause);
    if (rStr) {
        setProperty(propName, rStr);
        rStr->release();
    }
}

void CS35L41Driver::snapshotPlayback(AmplifierState& amp) {
    uint32_t irq1Sts1 = 0, irq1Sts2 = 0, irq1Sts3 = 0, irq1Sts4 = 0;
    uint32_t pwrMgtSts = 0;

    readRegister(amp, 0x00010010, &irq1Sts1);
    readRegister(amp, 0x00010014, &irq1Sts2);
    readRegister(amp, 0x00010018, &irq1Sts3);
    readRegister(amp, 0x0001001C, &irq1Sts4);
    readRegister(amp, 0x00002908, &pwrMgtSts);

    CIRRUS_LOG("playback interrupts snapshot for %s: irq1=0x%08X irq2=0x%08X irq3=0x%08X irq4=0x%08X power_status=0x%08X", amp.name,
               irq1Sts1, irq1Sts2, irq1Sts3, irq1Sts4, pwrMgtSts);
}

void CS35L41Driver::snapshotDiagnostics(AmplifierState& amp, const char* stage) {
    // This sweep feeds routine logs only. Essential power, DSP and protection
    // checks run at their call sites in either build. Avoid delaying the next
    // stereo endpoint with dozens of I2C reads when nobody requested verbosity.
    if (!gCirrusDebug)
        return;
    uint32_t irq1[4] = {0}, irq1Mask[4] = {0};
    uint32_t irq2[4] = {0}, irq2Mask[4] = {0};
    uint32_t spEn = 0, spRate = 0, spFmt = 0, spHiz = 0;
    uint32_t pwrSts = 0, strmErr = 0;

    CIRRUS_LOG("diagnostics dump (%s) for amplifier %s:", stage, amp.name);

    readRegister(amp, 0x00010010, &irq1[0]);
    readRegister(amp, 0x00010014, &irq1[1]);
    readRegister(amp, 0x00010018, &irq1[2]);
    readRegister(amp, 0x0001001C, &irq1[3]);

    uint32_t rawSts3 = 0;
    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegIrq1RawStatus3, &rawSts3);
    bool pupDone = (irq1[0] & 0x01000000) != 0;
    bool ampShort = (irq1[0] & 0x80000000) != 0;
    bool dspError = (irq1[0] & 0x00000002) != 0;
    bool pllLock = (rawSts3 & 0x00000002) != 0;

    CIRRUS_LOG("decoded interrupts on %s: pup_done=%d amp_short=%d dsp_err=%d pll_lock=%d", amp.name, pupDone, ampShort, dspError, pllLock);
    CIRRUS_LOG("irq1 status values on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X raw3=0x%08X", amp.name, irq1[0], irq1[1], irq1[2], irq1[3],
               rawSts3);

    readRegister(amp, 0x00010110, &irq1Mask[0]);
    readRegister(amp, 0x00010114, &irq1Mask[1]);
    readRegister(amp, 0x00010118, &irq1Mask[2]);
    readRegister(amp, 0x0001011C, &irq1Mask[3]);
    CIRRUS_LOG("irq1 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq1Mask[0], irq1Mask[1], irq1Mask[2],
               irq1Mask[3]);

    readRegister(amp, 0x00010810, &irq2[0]);
    readRegister(amp, 0x00010814, &irq2[1]);
    readRegister(amp, 0x00010818, &irq2[2]);
    readRegister(amp, 0x0001081C, &irq2[3]);
    CIRRUS_LOG("irq2 status registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2[0], irq2[1], irq2[2], irq2[3]);

    readRegister(amp, 0x00010910, &irq2Mask[0]);
    readRegister(amp, 0x00010914, &irq2Mask[1]);
    readRegister(amp, 0x00010918, &irq2Mask[2]);
    readRegister(amp, 0x0001091C, &irq2Mask[3]);
    CIRRUS_LOG("irq2 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2Mask[0], irq2Mask[1], irq2Mask[2],
               irq2Mask[3]);

    readRegister(amp, 0x00004800, &spEn);
    readRegister(amp, 0x00004804, &spRate);
    readRegister(amp, 0x00004808, &spFmt);
    readRegister(amp, 0x0000480C, &spHiz);
    CIRRUS_LOG("serial port configuration for %s: enables=0x%08X rate=0x%08X", amp.name, spEn, spRate);
    CIRRUS_LOG("serial port format for %s: format=0x%08X hiz=0x%08X", amp.name, spFmt, spHiz);

    readRegister(amp, 0x00002908, &pwrSts);
    readRegister(amp, 0x02BC5A08, &strmErr);
    CIRRUS_LOG("power status on %s: power_mgt=0x%08X arb_error=0x%08X", amp.name, pwrSts, strmErr);

    uint32_t dspTs = 0, mdsyncRx = 0;
    readRegister(amp, 0x025C0800, &dspTs);
    readRegister(amp, 0x00003420, &mdsyncRx);
    CIRRUS_LOG("dsp execution registers on %s: timestamp=0x%08X mdsync=0x%08X", amp.name, dspTs, mdsyncRx);

    uint32_t dspMbox1 = 0, dspMbox2 = 0;
    readRegister(amp, 0x00013020, &dspMbox1);
    readRegister(amp, 0x00013004, &dspMbox2);
    CIRRUS_LOG("mailbox states on %s: mbox1=0x%08X mbox2=0x%08X", amp.name, dspMbox1, dspMbox2);

    for (uint32_t i = 0; i < amp.diagnosticControlCount; i++) {
        uint32_t val = 0;
        bool valid = readRegister(amp, amp.diagnosticControls[i].address, &val);
        CIRRUS_LOG("control register on %s: %s (0x%08X) valid=%d value=0x%08X", amp.name, amp.diagnosticControls[i].name,
                   amp.diagnosticControls[i].address, valid, val);
    }
}

void CS35L41Driver::logPowerSnapshot(AmplifierState& amp) {
    uint32_t pwrCtrl1 = 0, pwrCtrl2 = 0, pwrCtrl3 = 0;
    readRegister(amp, 0x00002014, &pwrCtrl1);
    readRegister(amp, 0x00002018, &pwrCtrl2);
    readRegister(amp, 0x0000201C, &pwrCtrl3);

    CIRRUS_LOG("power rails snapshot for %s: ctrl1=0x%08X ctrl2=0x%08X ctrl3=0x%08X", amp.name, pwrCtrl1, pwrCtrl2, pwrCtrl3);

    bool pass = true;

    if ((pwrCtrl1 & 1) != 0) {
        CIRRUS_ERR("global enable unexpectedly active while idle on %s", amp.name);
        pass = false;
    }
    if ((pwrCtrl2 & 1) != 0) {
        CIRRUS_ERR("amplifier stage unexpectedly enabled while idle on %s", amp.name);
        pass = false;
    }

    if (pass) {
        CIRRUS_LOG("idle power verify results: pass on %s (GLOBAL_EN safely off)", amp.name);
    } else {
        CIRRUS_ERR("power verify results: fail on %s", amp.name);
    }
}
}
