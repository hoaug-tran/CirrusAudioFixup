#include "Service/CirrusAudioFixupSupport.hpp"

void CirrusAudioFixup::logProviderInfo(IOService* provider) {
    if (!provider) {
        CIRRUS_ERR("provider is null");
        return;
    }

    const auto cpu = cirrus::support::readCPUIdentity();
    setProperty("Cirrus_CPU_Vendor", cirrus::support::cpuVendorName(cpu.vendor));
    setProperty("Cirrus_CPU_Vendor_ID", cpu.vendor);
    setProperty("Cirrus_CPU_Brand", cpu.brand);
    if (auto* root = IORegistryEntry::fromPath("IODeviceTree:/")) {
        if (auto* data = OSDynamicCast(OSData, root->getProperty("model"))) {
            char model[128]{};
            const size_t length = data->getLength() < sizeof(model) - 1 ? data->getLength() : sizeof(model) - 1;
            if (length && data->getBytesNoCopy()) {
                memcpy(model, data->getBytesNoCopy(), length);
                setProperty("Cirrus_Machine_Model", model);
            }
        }
        root->release();
    }
    setProperty("Cirrus_Provider_Name", provider->getName());
    setProperty("Cirrus_Provider_Class", provider->getMetaClass()->getClassName());
    char providerPath[256]{};
    int pathLength = sizeof(providerPath);
    if (provider->getPath(providerPath, &pathLength, gIOServicePlane))
        setProperty("Cirrus_Provider_Path", providerPath);
    IOLog("CirrusAudioFixup: ATTACH cpu=%s brand=%s provider=%s class=%s path=%s\n", cirrus::support::cpuVendorName(cpu.vendor), cpu.brand,
          provider->getName(), provider->getMetaClass()->getClassName(), providerPath);
    CIRRUS_LOG("provider name=%s class=%s", provider->getName(), provider->getMetaClass()->getClassName());

    OSObject* addrObj = provider->getProperty("i2cAddress");
    if (OSNumber* addr = OSDynamicCast(OSNumber, addrObj)) {
        CIRRUS_LOG("provider i2cAddress=0x%02X", addr->unsigned32BitValue());
    }

    OSObject* modeObj = provider->getProperty("Interrupt Mode");
    if (OSString* mode = OSDynamicCast(OSString, modeObj)) {
        CIRRUS_LOG("provider interrupt=%s", mode->getCStringNoCopy());
    }

    OSObject* pathObj = provider->getProperty("acpi-path");
    if (OSString* path = OSDynamicCast(OSString, pathObj)) {
        CIRRUS_LOG("provider acpi-path=%s", path->getCStringNoCopy());
    }
}

void CirrusAudioFixup::dumpProviderProperties(IOService* provider) {
    if (!gCirrusDebug)
        return;
    if (!provider) {
        return;
    }

    CIRRUS_LOG("provider properties follow");

    OSDictionary* properties = provider->getPropertyTable();
    if (!properties)
        return;
    OSCollectionIterator* iterator = OSCollectionIterator::withCollection(properties);
    if (!iterator) {
        CIRRUS_ERR("property iterator failed");
        return;
    }

    while (OSObject* key = iterator->getNextObject()) {
        OSString* keyString = OSDynamicCast(OSString, key);
        if (!keyString) {
            continue;
        }

        OSObject* value = properties->getObject(keyString);
        if (!value) {
            continue;
        }

        if (OSString* str = OSDynamicCast(OSString, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), str->getCStringNoCopy());
        } else if (OSNumber* num = OSDynamicCast(OSNumber, value)) {
            CIRRUS_LOG("property %s=0x%llX", keyString->getCStringNoCopy(), num->unsigned64BitValue());
        } else if (OSBoolean* boo = OSDynamicCast(OSBoolean, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), boo->getValue() ? "true" : "false");
        } else {
            CIRRUS_LOG("property %s class=%s", keyString->getCStringNoCopy(), value->getMetaClass()->getClassName());
        }
    }

    iterator->release();
}

void CirrusAudioFixup::initTraceBuffer() {
    if (mTraceLock) {
        IOLockLock(mTraceLock);
        mTraceHead = 0;
        mTraceTail = 0;
        memset(&mTraceStats, 0, sizeof(mTraceStats));
        IOLockUnlock(mTraceLock);
    }
}

void CirrusAudioFixup::recordTrace(TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen,
                                   IOReturn ret) {
    if (!mTraceLock)
        return;

    uint64_t time = 0;
    clock_get_uptime(&time);
    uint64_t timeMs = 0;
    absolutetime_to_nanoseconds(time, &timeMs);
    timeMs /= 1000000;

    IOLockLock(mTraceLock);

    if (ret == kIOReturnOffline) {
        mTraceStats.noackCount++;
    } else if (ret == kIOReturnTimeout) {
        mTraceStats.retries++;
    }

    if (isBulk) {
        if (ret == kIOReturnSuccess)
            mTraceStats.bulkSuccess++;
        else
            mTraceStats.bulkFail++;
    } else {
        if (isWrite) {
            if (ret == kIOReturnSuccess)
                mTraceStats.writeSuccess++;
            else
                mTraceStats.writeFail++;
        } else {
            if (ret == kIOReturnSuccess)
                mTraceStats.readSuccess++;
            else
                mTraceStats.readFail++;
        }
    }

    mTraceBuffer[mTraceTail].timestamp = timeMs;
    mTraceBuffer[mTraceTail].amp = ampIndex;
    mTraceBuffer[mTraceTail].isWrite = isWrite;
    mTraceBuffer[mTraceTail].isBulk = isBulk;
    mTraceBuffer[mTraceTail].reg = reg;
    mTraceBuffer[mTraceTail].value = valOrLen;
    mTraceBuffer[mTraceTail].ret = ret;
    mTraceBuffer[mTraceTail].source = source;

    mTraceTail = (mTraceTail + 1) % kTraceBufferSize;
    if (mTraceTail == mTraceHead) {
        mTraceHead = (mTraceHead + 1) % kTraceBufferSize;
    }

    IOLockUnlock(mTraceLock);
}

void CirrusAudioFixup::publishStatistics() {
    if (!mTraceLock)
        return;
    IOLockLock(mTraceLock);
    const auto stats = mTraceStats;
    IOLockUnlock(mTraceLock);
    setProperty("Cirrus_Read_Success", stats.readSuccess, 32);
    setProperty("Cirrus_Read_Fail", stats.readFail, 32);
    setProperty("Cirrus_Write_Success", stats.writeSuccess, 32);
    setProperty("Cirrus_Write_Fail", stats.writeFail, 32);
    setProperty("Cirrus_Bulk_Success", stats.bulkSuccess, 32);
    setProperty("Cirrus_Bulk_Fail", stats.bulkFail, 32);
    setProperty("Cirrus_Offline_Return_Count", stats.noackCount, 32);
    setProperty("Cirrus_Timeout_Return_Count", stats.retries, 32);
    // Retain the legacy property for collectors; Offline is not electrical NACK proof.
    setProperty("Cirrus_NOACK_Count", stats.noackCount, 32);
}

const char* CirrusAudioFixup::stageName(DriverStage stage) {
    switch (stage) {
    case STAGE_PROBE:
        return "PROBE";
    case STAGE_RESET:
        return "RESET";
    case STAGE_OTP_BOOT:
        return "OTP_BOOT";
    case STAGE_ERRATA:
        return "ERRATA";
    case STAGE_CLOCK:
        return "CLOCK";
    case STAGE_ASP:
        return "ASP";
    case STAGE_GPIO:
        return "GPIO";
    case STAGE_PLATFORM:
        return "PLATFORM";
    case STAGE_FIRMWARE_DISCOVERY:
        return "FIRMWARE_DISCOVERY";
    case STAGE_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case STAGE_DSP_BOOT:
        return "DSP_BOOT";
    case STAGE_IDLE_VERIFY:
        return "IDLE_VERIFY";
    case STAGE_HDA_DETECT:
        return "HDA_DETECT";
    case STAGE_PLAYBACK_OPEN:
        return "PLAYBACK_OPEN";
    case STAGE_PLAYBACK_PREPARE:
        return "PLAYBACK_PREPARE";
    case STAGE_PLAYBACK_ACTIVE:
        return "PLAYBACK_ACTIVE";
    case STAGE_PLAYBACK_CLEANUP:
        return "PLAYBACK_CLEANUP";
    case STAGE_PLAYBACK_CLOSE:
        return "PLAYBACK_CLOSE";
    case STAGE_SAFE_IDLE:
        return "SAFE_IDLE";
    default:
        return "NONE";
    }
}

const char* CirrusAudioFixup::failureName(DiagnosticFailure failure) { return cirrus::diagnostics::failureName(failure); }

void CirrusAudioFixup::setDiagnosticStage(AmplifierState& amp, DriverStage stage) {
    DriverStage previous = amp.diagnostic.stage;
    amp.diagnostic.stage = stage;
    if (previous != stage) {
        CIRRUS_LOG("DIAG_STAGE amp=%s from=%s to=%s", amp.name, stageName(previous), stageName(stage));
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_Stage_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
}

void CirrusAudioFixup::markDiagnosticSuccess(AmplifierState& amp, DriverStage stage) {
    amp.diagnostic.stage = stage;
    amp.diagnostic.lastGoodStage = stage;
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_LastGood_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
    CIRRUS_LOG("DIAG_PASS amp=%s stage=%s", amp.name, stageName(stage));
    setDiagnosticStage(amp, stage);
}

void CirrusAudioFixup::recordDiagnosticFailure(AmplifierState& amp, DiagnosticFailure failure, UInt32 reg, UInt32 expected, UInt32 actual,
                                               IOReturn ioReturn, bool captureSnapshot) {
    DiagnosticState& diag = amp.diagnostic;
    bool first = diag.firstFailure == DIAG_OK;
    bool changed =
        diag.latestFailure != failure || diag.reg != reg || diag.actual != actual || diag.expected != expected || diag.ioReturn != ioReturn;
    if (first) {
        diag.firstFailure = failure;
        diag.firstStage = diag.stage;
        diag.firstReg = reg;
        diag.firstExpected = expected;
        diag.firstActual = actual;
        diag.firstIOReturn = ioReturn;
        char key[96];
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstStage_%s", amp.name);
        setProperty(key, stageName(diag.firstStage));
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstReg_%s", amp.name);
        setProperty(key, reg, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstExpected_%s", amp.name);
        setProperty(key, expected, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstActual_%s", amp.name);
        setProperty(key, actual, 32);
        snprintf(key, sizeof(key), "Cirrus_Diag_FirstIOReturn_%s", amp.name);
        setProperty(key, uint64_t(uint32_t(ioReturn)), 32);
    }
    diag.latestFailure = failure;
    if (diag.failureCount != UINT32_MAX)
        ++diag.failureCount;
    diag.reg = reg;
    diag.expected = expected;
    diag.actual = actual;
    diag.ioReturn = ioReturn;

    bool publishNow = first || changed || ((diag.failureCount % 32U) == 0);
    if (publishNow) {
        char property[96];
        OSString* value = nullptr;
        snprintf(property, sizeof(property), "Cirrus_Diag_FirstFailure_%s", amp.name);
        value = OSString::withCString(failureName(diag.firstFailure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_LatestFailure_%s", amp.name);
        value = OSString::withCString(failureName(failure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureCount_%s", amp.name);
        setProperty(property, diag.failureCount, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureReg_%s", amp.name);
        setProperty(property, reg, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Expected_%s", amp.name);
        setProperty(property, expected, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Actual_%s", amp.name);
        setProperty(property, actual, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_IOReturn_%s", amp.name);
        setProperty(property, uint64_t(uint32_t(ioReturn)), 32);
        publishStatistics();
    }

    if (first || changed) {
        CIRRUS_ERR("DIAG_FAIL amp=%s stage=%s code=%s count=%u reg=0x%08X expected=0x%08X actual=0x%08X io=0x%08X first=%s", amp.name,
                   stageName(diag.stage), failureName(failure), diag.failureCount, reg, expected, actual, ioReturn,
                   failureName(diag.firstFailure));
    }

    if (gCirrusDebug && !mCapturingFailureSnapshot && (first || (captureSnapshot && diag.snapshotFailure != failure))) {
        char latestTrace[80], firstTrace[80];
        snprintf(latestTrace, sizeof(latestTrace), "Cirrus_Trace_Latest_%s", amp.name);
        snprintf(firstTrace, sizeof(firstTrace), "Cirrus_Trace_First_%s", amp.name);
        // Freeze transfer history before snapshot reads add diagnostic traffic.
        dumpTraceBuffer(latestTrace, first ? firstTrace : nullptr);
        if (captureSnapshot && diag.snapshotFailure != failure) {
            diag.snapshotFailure = failure;
            captureFailureSnapshot(amp, failure);
        }
    }
}

bool CirrusAudioFixup::publishRuntimeStatus() {
    const uint32_t count = uint32_t(mAmplifierManager.count());
    const uint32_t stage = uint32_t(mAmplifierManager.stage());
    bool changed = count != mPublishedRuntimeCount || stage != mPublishedRuntimeStage;
    const uint32_t previousCount = mPublishedRuntimeCount == UINT32_MAX ? 0 : mPublishedRuntimeCount;
    if (changed) {
        setProperty("Cirrus_Runtime_Stage", uint64_t(stage), 32);
        setProperty("Cirrus_Runtime_Count", uint64_t(count), 32);
        mPublishedRuntimeCount = count;
        mPublishedRuntimeStage = stage;
    }
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        const auto* status = mAmplifierManager.status(i);
        if (!status)
            continue;
        cirrus::diagnostics::RuntimeDiagnosticSnapshot next{uint32_t(status->stage),         uint32_t(status->firstFailureStage),
                                                            uint32_t(status->firstFailure),  uint32_t(status->latestFailure),
                                                            uint32_t(status->cleanupResult), status->failures};
        if (!mRuntimePublished[i].update(next))
            continue;
        changed = true;
        char key[64]{};
        const char* names[] = {"Stage", "FailureStage", "FirstFailure", "LatestFailure", "CleanupReturn", "FailureCount"};
        const uint32_t values[] = {next.stage, next.failureStage, next.firstFailure, next.latestFailure, next.cleanup, next.failures};
        for (size_t field = 0; field < 6; ++field) {
            snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_%s", i, names[field]);
            setProperty(key, uint64_t(values[field]), 32);
        }
        snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Bound", i);
        setProperty(key, uint64_t(1), 32);
    }
    for (size_t i = count; i < previousCount; ++i) {
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Bound", i);
        setProperty(key, uint64_t(0), 32);
        if (const auto* released = mAmplifierManager.releasedStatus(i)) {
            const char* names[] = {"Stage", "FailureStage", "FirstFailure", "LatestFailure", "CleanupReturn", "FailureCount"};
            const uint32_t values[] = {uint32_t(released->stage),         uint32_t(released->firstFailureStage),
                                       uint32_t(released->firstFailure),  uint32_t(released->latestFailure),
                                       uint32_t(released->cleanupResult), released->failures};
            for (size_t field = 0; field < 6; ++field) {
                snprintf(key, sizeof(key), "Cirrus_Runtime_%lu_Released_%s", i, names[field]);
                setProperty(key, uint64_t(values[field]), 32);
            }
        }
        mRuntimePublished[i] = {};
    }
    if (changed)
        publishStatistics();
    return changed;
}

void CirrusAudioFixup::publishDriverVerdict() {
    publishRuntimeStatus();
    if (mPlatformIdentityPending) {
        setProperty("Cirrus_Driver_Verdict", "WAITING_BOARD_IDENTITY");
        return;
    }
    using cirrus::core::AmplifierStage;
    switch (mAmplifierManager.stage()) {
    case AmplifierStage::Ready:
        mAttachment.amplifier = "READY";
        break;
    case AmplifierStage::Playing:
        mAttachment.amplifier = "PLAYING";
        break;
    case AmplifierStage::Faulted:
        mAttachment.amplifier = "FAULTED";
        break;
    case AmplifierStage::Waiting:
        mAttachment.amplifier = "WAITING_RESOURCES";
        break;
    case AmplifierStage::DebugHalted:
        mAttachment.amplifier = "DEBUG_HALTED";
        break;
    case AmplifierStage::Suspended:
        mAttachment.amplifier = "SUSPENDED";
        break;
    default:
        break;
    }
    publishAttachmentStatus();
    bool topologyComplete = mPlatformProfile && mAmplifierCount == expectedAmplifierCount();
    for (size_t i = 0; i < mAmplifierCount; ++i)
        topologyComplete &= mAmplifiers[i].present;
    bool allInitialized = (mAmplifierCount > 0);
    bool anyFaulted = false, identityPending = false;
    const char* readyVerdict = nullptr;
    const char* waitingVerdict = "WAITING_FOR_RESOURCES";
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        const auto report = mAmplifierManager.report(i);
        const auto* status = mAmplifierManager.status(i);
        const bool ready =
            status && (status->stage == cirrus::core::AmplifierStage::Ready || status->stage == cirrus::core::AmplifierStage::Playing);
        allInitialized &= ready && (!report.managed || report.initialized);
        anyFaulted |= mAmplifiers[i].playbackFaulted || report.faulted;
        identityPending |= report.waiting;
        if (report.waitingVerdict)
            waitingVerdict = report.waitingVerdict;
        const char* label = report.readyVerdict ? report.readyVerdict : "READY_DRIVERS";
        if (!readyVerdict)
            readyVerdict = label;
        else if (strcmp(readyVerdict, label))
            readyVerdict = "READY_DRIVERS";
    }
    anyFaulted |= mAmplifierManager.stage() == cirrus::core::AmplifierStage::Faulted;
    if (anyFaulted)
        mAttachment.amplifier = "FAULTED";
    publishAttachmentStatus();
    const char* verdict = !mPowerAvailable              ? "SUSPENDED"
                          : bootArgEnabled("-cirrusro") ? "READ_ONLY"
                          : !mPlatformAuthorized        ? "UNSUPPORTED_PLATFORM"
                          : anyFaulted                  ? "FAULT_LATCHED"
                          : !topologyComplete           ? "INCOMPLETE_TOPOLOGY"
                          : identityPending             ? waitingVerdict
                          : mDebugPhaseHalted           ? "DEBUG_PHASE_HALTED"
                          : !allInitialized             ? "FAILED_INIT"
                          : readyVerdict                ? readyVerdict
                                                        : "READY_DRIVERS";
    OSString* value = OSString::withCString(verdict);
    if (value) {
        setProperty("Cirrus_Driver_Verdict", value);
        value->release();
    }
    for (size_t i = 0; i < mAmplifierCount; ++i)
        CIRRUS_LOG("DIAG_VERDICT driver=%s amp=%s first=%s latest=%s last_good=%s", verdict, mAmplifiers[i].name,
                   failureName(mAmplifiers[i].diagnostic.firstFailure), failureName(mAmplifiers[i].diagnostic.latestFailure),
                   stageName(mAmplifiers[i].diagnostic.lastGoodStage));
}

void CirrusAudioFixup::dumpTraceBuffer(const char* propertyName, const char* mirrorPropertyName) {
    if (!mTraceLock)
        return;

    CIRRUS_LOG("--- TRACE BUFFER DUMP START ---");

    size_t bufferSize = 128 * 1024;
    char* dumpBuffer = (char*)IOMallocData(bufferSize);
    if (!dumpBuffer) {
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_text return=NO_MEMORY");
        return;
    }
    auto* entries = static_cast<TraceEntry*>(IOMallocData(sizeof(TraceEntry) * kTraceBufferSize));
    if (!entries) {
        IOFreeData(dumpBuffer, bufferSize);
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_snapshot return=NO_MEMORY");
        return;
    }
    // Copy a bounded history while locked; allocate, format and publish outside
    // the transfer recorder lock. Diagnostic output must not serialize bus work.
    size_t count = 0;
    IOLockLock(mTraceLock);
    uint32_t cursor = mTraceHead;
    while (cursor != mTraceTail && count < kTraceBufferSize) {
        entries[count++] = mTraceBuffer[cursor];
        cursor = (cursor + 1) % kTraceBufferSize;
    }
    IOLockUnlock(mTraceLock);
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;
    char lineBuffer[128];

    for (size_t i = 0; i < count; ++i) {
        const TraceEntry& e = entries[i];
        const char* srcStr = "OTHER";
        switch (e.source) {
        case TRACE_PROBE:
            srcStr = "Probe";
            break;
        case TRACE_DUMP:
            srcStr = "Dump";
            break;
        case TRACE_CONSISTENCY:
            srcStr = "Consist";
            break;
        case TRACE_FIRMWARE:
            srcStr = "Firmware";
            break;
        case TRACE_PLAYBACK:
            srcStr = "Playback";
            break;
        default:
            break;
        }

        char nativeLabel[16]{};
        snprintf(nativeLabel, sizeof(nativeLabel), "AMP%u", e.amp);
        const char* ampStr = nativeLabel;

        if (e.isBulk) {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] BULK %s 0x%05X len=%u ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.value, e.ret);
        } else {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] %s 0x%05X %s 0x%08X ret=0x%X\n", e.timestamp, srcStr, ampStr,
                     e.isWrite ? "WRITE" : "READ", e.reg, e.isWrite ? "<-" : "->", e.value, e.ret);
        }

        size_t lineLen = strlen(lineBuffer);
        if (currentLen + lineLen < bufferSize - 1) {
            memcpy(dumpBuffer + currentLen, lineBuffer, lineLen);
            currentLen += lineLen;
            dumpBuffer[currentLen] = 0;
        }
    }
    CIRRUS_LOG("--- TRACE BUFFER DUMP END ---");

    OSString* strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propertyName ? propertyName : "Cirrus_Trace_Dump", strObj);
        if (mirrorPropertyName)
            setProperty(mirrorPropertyName, strObj);
        strObj->release();
    } else {
        CIRRUS_ERR("DIAGNOSTICS_UNAVAILABLE component=trace_property return=NO_MEMORY");
    }
    IOFreeData(dumpBuffer, bufferSize);

    IOFreeData(entries, sizeof(TraceEntry) * kTraceBufferSize);
}

void CirrusAudioFixup::captureFailureSnapshot(AmplifierState& amp, DiagnosticFailure) {
    for (size_t i = 0; i < mAmplifierCount; ++i)
        if (&mAmplifiers[i] == &amp) {
            mAmplifierManager.captureDiagnostics(i);
            return;
        }
}

void CirrusAudioFixup::recordAttachmentFailure(const char* stage, const char* reason, IOReturn result, uint32_t index,
                                               const char* indexKind) {
    if (!stage)
        stage = "UNKNOWN";
    if (!reason)
        reason = "UNSPECIFIED_FAILURE";
    if (!indexKind)
        indexKind = "UNKNOWN";
#if defined(KERNEL)
    publishMatchingEvent(reason, mProvider, result, "Cirrus_Last_Attachment_Failure");
#endif
    const bool publish = mAttachment.record(stage, reason, result, index, indexKind);
    if (!publish)
        return;
    setProperty("Cirrus_Attach_First_Stage", mAttachment.first.stage);
    setProperty("Cirrus_Attach_First_Reason", mAttachment.first.reason);
    setProperty("Cirrus_Attach_First_Return", uint64_t(uint32_t(mAttachment.first.result)), 32);
    setProperty("Cirrus_Attach_First_Index_Kind", mAttachment.first.indexKind);
    setProperty("Cirrus_Attach_Latest_Index_Kind", indexKind);
    setProperty("Cirrus_Attach_First_Index", uint64_t(uint32_t(mAttachment.first.index)), 32);
    setProperty("Cirrus_Attach_Latest_Stage", mAttachment.latest.stage);
    setProperty("Cirrus_Attach_Latest_Reason", mAttachment.latest.reason);
    setProperty("Cirrus_Attach_Latest_Return", uint64_t(uint32_t(result)), 32);
    setProperty("Cirrus_Attach_Latest_Index", uint64_t(uint32_t(index)), 32);
    setProperty("Cirrus_Attach_Failure_Count", uint64_t(uint32_t(mAttachment.failureCount)), 32);
    char key[80]{};
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Stage", indexKind, index);
    setProperty(key, stage);
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Reason", indexKind, index);
    setProperty(key, reason);
    snprintf(key, sizeof(key), "Cirrus_Attach_%s_%u_Return", indexKind, index);
    setProperty(key, uint64_t(uint32_t(result)), 32);
    publishAttachmentStatus();
    CIRRUS_ERR("ATTACH_FAIL stage=%s reason=%s index_kind=%s index=%u return=0x%08X profile=%s board=0x%08X comm=%s amp=%s first=%s", stage,
               reason, indexKind, index, result, mPlatformProfile && mPlatformProfile->name ? mPlatformProfile->name : "unknown",
               mPlatformSubsystemId, mAttachment.communication, mAttachment.amplifier, mAttachment.first.reason);
}

void CirrusAudioFixup::publishAttachmentStatus() {
    setProperty("Cirrus_Communication_Status", mAttachment.communication);
    setProperty("Cirrus_Amplifier_Status", mAttachment.amplifier);
}
