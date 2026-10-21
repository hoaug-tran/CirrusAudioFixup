#include "Devices/Amplifiers/CS35L41/CS35L41Internal.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"
#include "Firmware/WMFW/WMFWUploader.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
bool protectionResourceAvailable(uint16_t vendor, uint16_t device, uint32_t speaker) {
    const auto* resource = selectFirmwareResource(cs35l41Firmware, cs35l41FirmwareCount, vendor, device, speaker);
    size_t left = 0, right = 0;
    return resource && resource->wmfw && resource->wmfwSize && !(resource->wmfwSize % 4) &&
           selectChannelCoefficients(*resource, false, left) && selectChannelCoefficients(*resource, true, right);
}


void CS35L41Driver::initializeFirmware(AmplifierState& amp, const char* phaseArg) {
    (void)phaseArg;
    CIRRUS_LOG("starting full initialization for amplifier: %s", amp.name);
    amp.firmwareValidated = false;
    amp.dspAlive = false;
    amp.monitorCount = 0;

    if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
        CIRRUS_ERR("firmware or tuning data missing for %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
        return;
    }

    bool wmfwUploaded = false;
    bool coefficientsUploaded = false;

    if (!stopDSP(amp)) {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.playbackFaulted = true;
        return;
    }
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;

    FirmwareImage* image = (FirmwareImage*)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD, 0, (UInt32)sizeof(FirmwareImage), 0, kIOReturnNoMemory, false);
        return;
    }

    if (!WMFWParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("wmfw parsing successful on %s. fw id: 0x%06X, expected algs: %u", amp.name, image->firmwareId, image->algorithmTotal);

    MappedImage* wmfwMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
    if (wmfwMapped) {
        if (WMFWMemoryMapper::mapFirmwareImage(*image, *wmfwMapped)) {
            CIRRUS_LOG("mapping wmfw image successful on %s, starting upload", amp.name);
            UploadSession session;
            DriverRegisterIO io(this, amp);
            wmfwUploaded = WMFWUploadScheduler::run(amp.name, io, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("failed to map wmfw image on %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    if (!wmfwUploaded) {
        CIRRUS_ERR("wmfw upload failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("parsing dsp algorithms for %s", amp.name);
    if (!parseDSPAlgorithms(amp, *image)) {
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    WMFWControlRef stateRef{}, heartbeatRef{};
    if (!WMFWParser::findControl(image, "HALO_STATE", stateRef) || !WMFWParser::findControl(image, "HALO_HEARTBEAT", heartbeatRef) ||
        (stateRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) ||
        (heartbeatRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) || stateRef.control->type != WMFW_ADSP2_XM ||
        stateRef.control->len != 4 || heartbeatRef.control->type != WMFW_ADSP2_XM || heartbeatRef.control->len != 4 ||
        !WMFWParser::resolveControl(*image, stateRef, amp.haloStateRegister) ||
        !WMFWParser::resolveControl(*image, heartbeatRef, amp.haloHeartbeatRegister)) {
        amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    CIRRUS_LOG("HALO controls on %s: state=0x%08X heartbeat=0x%08X firmware=0x%06X", amp.name, amp.haloStateRegister,
               amp.haloHeartbeatRegister, image->firmwareId);
    amp.firmwareIdVersion = image->haloFirmwareVersion;
    CIRRUS_LOG("HALO firmware ID version on %s: 0x%06X", amp.name, amp.firmwareIdVersion);
    char fwIdVersionProp[64];
    snprintf(fwIdVersionProp, sizeof(fwIdVersionProp), "Cirrus_FW_ID_Version_%s", amp.name);
    setProperty(fwIdVersionProp, (uint64_t)amp.firmwareIdVersion, 32);

    auto containsStr = [](const char* str, const char* sub) -> bool {
        if (!str || !sub)
            return false;
        size_t strLen = strlen(str);
        size_t subLen = strlen(sub);
        if (subLen > strLen)
            return false;
        for (size_t i = 0; i <= strLen - subLen; i++) {
            bool match = true;
            for (size_t j = 0; j < subLen; j++) {
                char c1 = str[i + j];
                char c2 = sub[j];
                if (c1 >= 'a' && c1 <= 'z')
                    c1 -= 32;
                if (c2 >= 'a' && c2 <= 'z')
                    c2 -= 32;
                if (c1 != c2) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    };

    amp.diagnosticControlCount = 0;
    for (uint32_t i = 0; i < image->wmfwControlCount; i++) {
        const WMFWControl& ctl = image->wmfwControls[i];
        if (containsStr(ctl.name, "PCM") || containsStr(ctl.name, "LEVEL") || containsStr(ctl.name, "PEAK") ||
            containsStr(ctl.name, "RMS") || containsStr(ctl.name, "STREAM") || containsStr(ctl.name, "ACTIVE")) {
            if (amp.diagnosticControlCount < 10) {
                WMFWControlRef ref{};
                uint32_t regAddress = 0;
                if (ctl.len == 4 && (ctl.type == WMFW_ADSP2_XM || ctl.type == WMFW_ADSP2_YM) &&
                    WMFWParser::findControl(image, ctl.name, ref) && WMFWParser::resolveControl(*image, ref, regAddress)) {
                    strlcpy(amp.diagnosticControls[amp.diagnosticControlCount].name, ctl.name, sizeof(amp.diagnosticControls[0].name));
                    amp.diagnosticControls[amp.diagnosticControlCount].address = regAddress;
                    CIRRUS_LOG("registered diagnostic control '%s' at 0x%08X on %s", ctl.name, regAddress, amp.name);
                    amp.diagnosticControlCount++;
                }
            }
        }
    }
    const char* calibrationControls[] = {"CAL_AMBIENT", "CAL_R", "CAL_STATUS", "CAL_CHECKSUM"};
    for (const char* name : calibrationControls) {
        WMFWControlRef ref{};
        uint32_t address = 0;
        if (WMFWParser::findControl(image, name, ref) && ref.algorithm->id == 0xCD && ref.control->type == WMFW_ADSP2_XM &&
            ref.control->len == 4 && WMFWParser::resolveControl(*image, ref, address) &&
            amp.diagnosticControlCount < sizeof(amp.diagnosticControls) / sizeof(amp.diagnosticControls[0])) {
            auto& control = amp.diagnosticControls[amp.diagnosticControlCount++];
            strlcpy(control.name, name, sizeof(control.name));
            control.address = address;
        }
    }
    char calibrationProperty[80];
    snprintf(calibrationProperty, sizeof(calibrationProperty), "Cirrus_Calibration_Status_%s", amp.name);
    OSString* calibrationStatus = OSString::withCString("NOT_APPLIED_BY_DRIVER");
    if (calibrationStatus) {
        setProperty(calibrationProperty, calibrationStatus);
        calibrationStatus->release();
    }
    CIRRUS_LOG("found %u matching diagnostic controls on %s", amp.diagnosticControlCount, amp.name);

    if (WMFWParser::parseBIN(amp.binData, amp.binSize, image)) {
        CIRRUS_LOG("bin parsing successful on %s, found %u coefficient blocks", amp.name, image->coefficientCount);
        MappedImage* coeffMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
        if (coeffMapped) {
            if (WMFWMemoryMapper::mapCoefficients(*image, *coeffMapped) && coeffMapped->regionCount > 0) {
                CIRRUS_LOG("starting coefficient file upload on %s", amp.name);
                UploadSession session;
                DriverRegisterIO io(this, amp);
                coefficientsUploaded = WMFWUploadScheduler::run(amp.name, io, *coeffMapped, session);
            } else {
                CIRRUS_ERR("coefficient mapping produced no uploadable regions on %s", amp.name);
                recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
            }
            IOFree(coeffMapped, sizeof(MappedImage));
        }
    } else {
        CIRRUS_ERR("bin file parsing failed on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
    }

    if (!coefficientsUploaded) {
        CIRRUS_ERR("coefficient upload failed on %s; keeping DSP stopped and output disabled", amp.name);
        if (amp.diagnostic.latestFailure != DIAG_COEFFICIENT_PARSE)
            recordDiagnosticFailure(amp, DIAG_COEFFICIENT_UPLOAD);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!applyCalibration(amp, image)) {
        CIRRUS_ERR("calibration failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_CALIBRATION);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    amp.firmwareValidated = true;

    CIRRUS_LOG("bringing up dsp controller on %s", amp.name);
    if (stopAfterDebugStage("firmware")) {
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    bool booted = bringUpDSP(amp);

    if (booted && verifyDSPAlive(amp)) {
        CIRRUS_LOG("dsp is successfully verified alive on %s", amp.name);
        amp.dspAlive = true;
        amp.monitorCount = 1;
    } else {
        CIRRUS_ERR("dsp bringup failed or dsp is unresponsive on %s; disabling DSP mode", amp.name);
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.firmwareValidated = false;
        amp.dspAlive = false;
        amp.monitorCount = 0;
        stopDSP(amp);
    }

    logDSPBootReport(amp);

    IOFree(image, sizeof(FirmwareImage));
    CIRRUS_LOG("full firmware init complete for amplifier %s", amp.name);
}

bool CS35L41Driver::applyCalibration(AmplifierState& amp, const FirmwareImage* image) {
    const char* fields[] = {"Status", "R0", "Ambient", "Valid", "Checksum", "Selection"};
    for (const char* field : fields) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_%s_%s", field, amp.name);
        removeProperty(property);
    }
    if (!image)
        return false;

    if (bootArgEnabled("-cirrusnocal")) {
        char calProp[80];
        snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* calVal = OSString::withCString("SKIPPED_BY_BOOT_ARG");
        if (calVal) {
            setProperty(calProp, calVal);
            calVal->release();
        }
        return true;
    }

    uint32_t addrAmbient = 0, addrR = 0, addrStatus = 0, addrChecksum = 0;
    WMFWControlRef refAmbient{}, refR{}, refStatus{}, refChecksum{};
    bool resolved = WMFWParser::findControl(image, "CAL_AMBIENT", refAmbient) && refAmbient.algorithm->id == 0xCD &&
                    refAmbient.control->type == WMFW_ADSP2_XM && refAmbient.control->len == 4 &&
                    WMFWParser::resolveControl(*image, refAmbient, addrAmbient) && WMFWParser::findControl(image, "CAL_R", refR) &&
                    refR.algorithm->id == 0xCD && refR.control->type == WMFW_ADSP2_XM && refR.control->len == 4 &&
                    WMFWParser::resolveControl(*image, refR, addrR) && WMFWParser::findControl(image, "CAL_STATUS", refStatus) &&
                    refStatus.algorithm->id == 0xCD && refStatus.control->type == WMFW_ADSP2_XM && refStatus.control->len == 4 &&
                    WMFWParser::resolveControl(*image, refStatus, addrStatus) &&
                    WMFWParser::findControl(image, "CAL_CHECKSUM", refChecksum) && refChecksum.algorithm->id == 0xCD &&
                    refChecksum.control->type == WMFW_ADSP2_XM && refChecksum.control->len == 4 &&
                    WMFWParser::resolveControl(*image, refChecksum, addrChecksum);

    if (!resolved) {
        CIRRUS_ERR("failed to resolve all 4 calibration controls in algorithm 0xCD on %s", amp.name);
        char calProp[80];
        snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* calVal = OSString::withCString("CONTROL_RESOLUTION_FAILED");
        if (calVal) {
            setProperty(calProp, calVal);
            calVal->release();
        }
        return false;
    }

    uint8_t ampIdx = (amp.channel == cirrus::core::AudioChannel::Right ||
                      amp.address == cirrus::devices::amplifiers::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    bool foundData = false;
    bool invalidData = false;
    bool uidReadFailed = false;
    int32_t ambientVal = 0;
    uint32_t statusVal = 1;
    uint32_t r0Val = 0;
    uint32_t checksumVal = 0;
    const char* sourceStr = "NONE";
    auto publishStatus = [&](const char* status) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* value = OSString::withCString(status);
        if (value) {
            setProperty(property, value);
            value->release();
        }
    };

    IORegistryEntry* options = IORegistryEntry::fromPath("IODeviceTree:/options");
    if (!options) {
        options = IORegistryEntry::fromPath("IODeviceTree:/chosen");
    }
    if (options) {
        OSObject* prop = options->copyProperty("02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData");
        if (!prop) {
            prop = options->copyProperty("CirrusSmartAmpCalibrationData");
        }
        invalidData = prop != nullptr;
        OSData* calData = OSDynamicCast(OSData, prop);
        if (calData) {
            uint32_t len = calData->getLength();
            const uint8_t* rawBytes = (const uint8_t*)calData->getBytesNoCopy();
            if (rawBytes && len >= sizeof(cs35l41_amp_efi_data)) {
                const cs35l41_amp_efi_data* efiData = (const cs35l41_amp_efi_data*)rawBytes;
                uint32_t count = OSSwapLittleToHostInt32(efiData->count);
                if (count <= (len - sizeof(cs35l41_amp_efi_data)) / sizeof(cs35l41_amp_cal_data)) {
                    invalidData = false;
                    const cs35l41_amp_cal_data* selected = nullptr;
                    uint32_t selectedIndex = UINT32_MAX;
                    const char* selectionReason = "NO_MATCHING_EFI_RECORD";
                    auto targetOf = [](const cs35l41_amp_cal_data& entry) -> uint64_t {
                        return ((uint64_t)OSSwapLittleToHostInt32(entry.calTarget[1]) << 32) | OSSwapLittleToHostInt32(entry.calTarget[0]);
                    };
                    bool hasTarget = false;
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto& entry = efiData->data[i];
                        if ((entry.calTime[0] || entry.calTime[1]) && targetOf(entry))
                            hasTarget = true;
                    }

                    // Linux assembles the silicon UID from DIE_STS2 (high)
                    // and DIE_STS1 (low). Only targeted EFI records need these
                    // reads; a wildcard-only table can be selected by index.
                    uint64_t siliconUid = 0;
                    if (hasTarget) {
                        uint32_t high = 0, low = 0;
                        uidReadFailed =
                            !readRegister(amp, 0x00017044, &high, TRACE_FIRMWARE) || !readRegister(amp, 0x00017040, &low, TRACE_FIRMWARE);
                        siliconUid = ((uint64_t)high << 32) | low;
                        if (!uidReadFailed && siliconUid) {
                            for (uint32_t i = 0; i < count; ++i) {
                                const auto& entry = efiData->data[i];
                                if ((entry.calTime[0] || entry.calTime[1]) && targetOf(entry) == siliconUid) {
                                    selected = &entry;
                                    selectedIndex = i;
                                    selectionReason = "EXACT_SILICON_UID";
                                    break;
                                }
                            }
                        }
                    }

                    // An unmatched nonzero target must not silently become
                    // another speaker's calibration. Linux permits index
                    // fallback only when the entry or silicon UID is zero.
                    if (!uidReadFailed && !selected && ampIdx < count) {
                        const auto& entry = efiData->data[ampIdx];
                        if ((entry.calTime[0] || entry.calTime[1]) && (!targetOf(entry) || !siliconUid)) {
                            selected = &entry;
                            selectedIndex = ampIdx;
                            selectionReason = "INDEX_FALLBACK_ZERO_UID_OR_TARGET";
                        }
                    }
                    char selection[256]{}, selectionKey[80]{};
                    snprintf(selection, sizeof(selection), "reason=%s records=%u index=%u uid=0x%016llx target=0x%016llx uid_read_required=%u",
                             uidReadFailed ? "UID_READ_FAILED" : selectionReason, count, selectedIndex,
                             static_cast<unsigned long long>(siliconUid),
                             static_cast<unsigned long long>(selected ? targetOf(*selected) : 0), unsigned(hasTarget));
                    snprintf(selectionKey, sizeof(selectionKey), "Cirrus_Calibration_Selection_%s", amp.name);
                    if (auto* value = OSString::withCString(selection)) {
                        setProperty(selectionKey, value);
                        value->release();
                    }
                    if (selected) {
                        ambientVal = selected->calAmbient;
                        statusVal = selected->calStatus;
                        r0Val = OSSwapLittleToHostInt16(selected->calR);
                        checksumVal = r0Val + 1;
                        foundData = true;
                        sourceStr = "APPLIED_FROM_EFI_VARIABLE";
                    }
                }
            }
        }
        if (prop)
            prop->release();
        options->release();
    }
    if (uidReadFailed) {
        publishStatus("UID_READ_FAILED");
        return false;
    }
    if (invalidData) {
        publishStatus("INVALID_EFI_DATA");
        return false;
    }

    if (!foundData) {
        uint32_t bootR0 = 0;
        const char* r0Arg = (ampIdx == 0) ? "-cirruscalr0l" : "-cirruscalr0r";
        if (parseBootArgument(r0Arg, &bootR0, sizeof(bootR0)) || parseBootArgument("-cirruscalr0", &bootR0, sizeof(bootR0))) {
            r0Val = bootR0;
            statusVal = 1;
            ambientVal = 0;
            parseBootArgument("-cirruscalstatus", &statusVal, sizeof(statusVal));
            bool hasAmbient = parseBootArgument("-cirruscalambient", &ambientVal, sizeof(ambientVal));
            if (!hasAmbient || !r0Val || r0Val > 0xFFFFU || statusVal > 0xFFU || ambientVal < -128 || ambientVal > 127) {
                publishStatus("INVALID_BOOT_ARG_DATA");
                return false;
            }
            checksumVal = r0Val + 1;
            foundData = true;
            sourceStr = "APPLIED_FROM_BOOT_ARG";
        }
    }

    if (!foundData) {
        publishStatus("NOT_AVAILABLE");
        return true;
    }

    bool ok = writeRegister(amp, addrAmbient, (uint32_t)ambientVal, TRACE_FIRMWARE) && writeRegister(amp, addrR, r0Val, TRACE_FIRMWARE) &&
              writeRegister(amp, addrStatus, statusVal, TRACE_FIRMWARE) && writeRegister(amp, addrChecksum, checksumVal, TRACE_FIRMWARE);

    uint32_t rbAmbient = 0, rbR = 0, rbStatus = 0, rbChecksum = 0;
    bool verified = ok && readRegister(amp, addrAmbient, &rbAmbient, TRACE_FIRMWARE) && readRegister(amp, addrR, &rbR, TRACE_FIRMWARE) &&
                    readRegister(amp, addrStatus, &rbStatus, TRACE_FIRMWARE) &&
                    readRegister(amp, addrChecksum, &rbChecksum, TRACE_FIRMWARE) && rbAmbient == (uint32_t)ambientVal && rbR == r0Val &&
                    rbStatus == statusVal && rbChecksum == checksumVal;

    char calProp[80];
    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
    OSString* calVal = OSString::withCString(verified ? sourceStr : "VERIFY_FAILED");
    if (calVal) {
        setProperty(calProp, calVal);
        calVal->release();
    }

    if (!verified) {
        CIRRUS_ERR("calibration verification failed on %s: ok=%d verified=%d", amp.name, ok, verified);
        return false;
    }

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_R0_%s", amp.name);
    setProperty(calProp, (uint64_t)r0Val, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Ambient_%s", amp.name);
    setProperty(calProp, (uint64_t)ambientVal, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Valid_%s", amp.name);
    setProperty(calProp, (uint64_t)statusVal, 32);

    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Checksum_%s", amp.name);
    setProperty(calProp, (uint64_t)checksumVal, 32);

    CIRRUS_LOG("calibration applied and verified on %s from %s: R0=%u, Ambient=%d, Status=%u, Checksum=%u", amp.name, sourceStr, r0Val,
               ambientVal, statusVal, checksumVal);
    return true;
}

bool CS35L41Driver::sendMailboxCommand(AmplifierState& amp, UInt32 command, UInt32 expectedStatus) {
    if (!writeRegister(amp, 0x00013020, command, TRACE_PLAYBACK)) {
        recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, 0x00013020, command, 0, mLastTransferReturn);
        return false;
    }

    UInt32 status = 0;
    // Check immediately, then retain the full five-millisecond retry window.
    // Sleep only while the command is pending.
    for (unsigned attempt = 0; attempt <= 5; ++attempt) {
        if (attempt != 0)
            IOSleep(1);
        if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kDspMbox2Register, &status, TRACE_PLAYBACK)) {
            recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::amplifiers::cs35l41::registers::kDspMbox2Register,
                                    expectedStatus, status, mLastTransferReturn);
            return false;
        }
        if (status == 0xFFFFFFFFU || status == 0x00FFFFFFU) {
            CIRRUS_ERR("DSP mailbox reported error on %s for command %u: 0x%08X", amp.name, command, status);
            recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::amplifiers::cs35l41::registers::kDspMbox2Register,
                                    expectedStatus, status);
            return false;
        }
        if (status == expectedStatus)
            return true;
    }

    CIRRUS_ERR("DSP mailbox command %u timed out on %s (expected=%u status=0x%08X)", command, amp.name, expectedStatus, status);
    recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::amplifiers::cs35l41::registers::kDspMbox2Register, expectedStatus,
                            status);
    return false;
}

bool CS35L41Driver::stopDSP(AmplifierState& amp) {
    DriverRegisterIO io(this, amp);
    return cirrus::devices::amplifiers::cs35l41::DSPControl::halt(io, amp, [](uint32_t delay) { IODelay(delay); });
}

void CS35L41Driver::discoverFirmware(AmplifierState& amp) {
    CIRRUS_LOG("discovering firmware for amplifier: %s", amp.name);

    amp.firmwareValidated = false;
    amp.tuningPcmGain = 17;
    char tuningProperty[80];
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_Source_%s", amp.name);
    removeProperty(tuningProperty);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_PCM_Gain_%s", amp.name);
    removeProperty(tuningProperty);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_CRC32_%s", amp.name);
    removeProperty(tuningProperty);
    amp.wmfwData = amp.binData = nullptr;
    amp.wmfwSize = amp.binSize = 0;

    char propStatus[64];
    snprintf(propStatus, sizeof(propStatus), "Cirrus_Phase5A_Status_%s", amp.name);

    if (bootArgEnabled("-cirrusnodsp")) {
        CIRRUS_LOG("Non-DSP Bypass mode forced via boot-arg for %s", amp.name);
        OSString* statusStr = OSString::withCString("BYPASS");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    uint32_t subVendor = 0;
    uint32_t subDevice = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t revisionId = 0;

    struct SsidTranslationQuirk {
        uint16_t fromVendor;
        uint16_t fromDevice;
        uint16_t toVendor;
        uint16_t toDevice;
    };
    static const SsidTranslationQuirk kSsidQuirks[] = {
        {0x17AA, 0x382B, 0x17AA, 0x3847},
    };
    auto firmwareResourceExists = [](uint16_t vendor, uint16_t device) -> bool {
        for (size_t i = 0; i < cs35l41FirmwareCount; ++i) {
            if (cs35l41Firmware[i].subsystemVendor == vendor && cs35l41Firmware[i].subsystemDevice == device)
                return true;
        }
        for (size_t i = 0; i < sizeof(kSsidQuirks) / sizeof(kSsidQuirks[0]); ++i) {
            if (kSsidQuirks[i].fromVendor == vendor && kSsidQuirks[i].fromDevice == device)
                return true;
            if (kSsidQuirks[i].toVendor == vendor && kSsidQuirks[i].toDevice == device)
                return true;
        }
        return false;
    };

    IOService* audioController = getAudioController();
    if (audioController) {
        IOPCIDevice* pciDev =
#if defined(KERNEL)
            audioController ? static_cast<IOPCIDevice*>(audioController->metaCast("IOPCIDevice")) : nullptr
#else
            OSDynamicCast(IOPCIDevice, audioController)
#endif
            ;
        if (pciDev) {
            vendorId = pciDev->configRead16(kIOPCIConfigVendorID);
            deviceId = pciDev->configRead16(kIOPCIConfigDeviceID);
            subVendor = pciDev->configRead16(kIOPCIConfigSubSystemVendorID);
            subDevice = pciDev->configRead16(kIOPCIConfigSubSystemID);
            revisionId = pciDev->configRead8(kIOPCIConfigRevisionID);
        }

        if (subVendor == 0 || subDevice == 0) {
            OSData* subVenData = OSDynamicCast(OSData, audioController->getProperty("subsystem-vendor-id"));
            if (subVenData && subVenData->getLength() >= 4)
                subVendor = *((uint32_t*)subVenData->getBytesNoCopy()) & 0xFFFF;
            else if (subVenData && subVenData->getLength() >= 2)
                subVendor = *((uint16_t*)subVenData->getBytesNoCopy());

            OSData* subDevData = OSDynamicCast(OSData, audioController->getProperty("subsystem-id"));
            if (subDevData && subDevData->getLength() >= 4) {
                uint32_t rawSub = *((uint32_t*)subDevData->getBytesNoCopy());
                if (subVendor == 0 && rawSub > 0xFFFF) {
                    uint16_t low = rawSub & 0xFFFF;
                    uint16_t high = rawSub >> 16;
                    if (firmwareResourceExists(low, high) && !firmwareResourceExists(high, low)) {
                        subVendor = low;
                        subDevice = high;
                    } else {
                        subVendor = high;
                        subDevice = low;
                    }
                } else {
                    subDevice = rawSub & 0xFFFF;
                }
            } else if (subDevData && subDevData->getLength() >= 2) {
                subDevice = *((uint16_t*)subDevData->getBytesNoCopy());
            }
        }

        if (vendorId == 0 || deviceId == 0) {
            OSData* venData = OSDynamicCast(OSData, audioController->getProperty("vendor-id"));
            if (venData && venData->getLength() >= 4)
                vendorId = *((uint32_t*)venData->getBytesNoCopy()) & 0xFFFF;
            else if (venData && venData->getLength() >= 2)
                vendorId = *((uint16_t*)venData->getBytesNoCopy());

            OSData* devData = OSDynamicCast(OSData, audioController->getProperty("device-id"));
            if (devData && devData->getLength() >= 4)
                deviceId = *((uint32_t*)devData->getBytesNoCopy()) & 0xFFFF;
            else if (devData && devData->getLength() >= 2)
                deviceId = *((uint16_t*)devData->getBytesNoCopy());
        }

        if (revisionId == 0) {
            OSData* revData = OSDynamicCast(OSData, audioController->getProperty("revision-id"));
            if (revData && revData->getLength() >= 4)
                revisionId = *((uint32_t*)revData->getBytesNoCopy());
            else if (revData && revData->getLength() >= 1)
                revisionId = *((uint8_t*)revData->getBytesNoCopy());
        }
        char propPath[64];
        snprintf(propPath, sizeof(propPath), "Cirrus_PCI_Path_%s", amp.name);
        char pathStr[512] = {0};
        int pathLen = sizeof(pathStr);
        if (audioController->getPath(pathStr, &pathLen, gIOServicePlane)) {
            OSString* pStr = OSString::withCString(pathStr);
            if (pStr) {
                setProperty(propPath, pStr);
                pStr->release();
            }
        }

        OSString* pciDebug = OSDynamicCast(OSString, audioController->getProperty("pcidebug"));
        if (pciDebug) {
            char propBDF[64];
            snprintf(propBDF, sizeof(propBDF), "Cirrus_PCI_BDF_%s", amp.name);
            setProperty(propBDF, pciDebug);
        }

        char prop[64];
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Vendor_%s", amp.name);
        setProperty(prop, (uint64_t)vendorId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Device_%s", amp.name);
        setProperty(prop, (uint64_t)deviceId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Revision_%s", amp.name);
        setProperty(prop, (uint64_t)revisionId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubVendor_%s", amp.name);
        setProperty(prop, (uint64_t)subVendor, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubDevice_%s", amp.name);
        setProperty(prop, (uint64_t)subDevice, 32);

        audioController->release();
    }

    uint32_t codecSSID = 0;
    const char* codecClasses[] = {"IOHDACodecFunction", "IOHDACodecDevice", "AppleHDACodecGeneric", "AppleHDACodec"};
    for (size_t c = 0; c < 4 && codecSSID == 0; c++) {
        OSDictionary* match = serviceMatching(codecClasses[c]);
        if (!match)
            continue;
        OSIterator* iter = getMatchingServices(match);
        match->release();
        if (!iter)
            continue;
        IORegistryEntry* entry;
        while ((entry = OSDynamicCast(IORegistryEntry, iter->getNextObject()))) {
            // Reject a known HDMI/GPU codec: its SSID is not speaker identity.
            uint32_t codecVendor = 0;
            IORegistryEntry* ancestor = entry;
            for (size_t depth = 0; ancestor && depth < 3 && !codecVendor; ++depth) {
                auto* vendor = OSDynamicCast(OSNumber, ancestor->getProperty("IOHDACodecVendorID"));
                if (!vendor)
                    vendor = OSDynamicCast(OSNumber, ancestor->getProperty("vendorcodecID"));
                if (vendor)
                    codecVendor = vendor->unsigned32BitValue();
                ancestor = ancestor->getParentEntry(gIOServicePlane);
            }
            if (codecVendor && (codecVendor >> 16) != 0x10EC)
                continue;
            OSData* subData = OSDynamicCast(OSData, entry->getProperty("subsystem-id"));
            if (!subData)
                subData = OSDynamicCast(OSData, entry->getProperty("SubsystemID"));
            if (!subData)
                subData = OSDynamicCast(OSData, entry->getProperty("IOHDACodecFunctionSubsystemID"));
            if (subData && subData->getLength() >= 4) {
                codecSSID = *(const uint32_t*)subData->getBytesNoCopy();
                if (codecSSID)
                    break;
            }
            OSNumber* subNum = OSDynamicCast(OSNumber, entry->getProperty("subsystem-id"));
            if (!subNum)
                subNum = OSDynamicCast(OSNumber, entry->getProperty("SubsystemID"));
            if (!subNum)
                subNum = OSDynamicCast(OSNumber, entry->getProperty("IOHDACodecFunctionSubsystemID"));
            if (subNum) {
                codecSSID = subNum->unsigned32BitValue();
                if (codecSSID)
                    break;
            }
        }
        iter->release();
    }

    uint32_t ssid = 0;
    if (codecSSID != 0) {
        ssid = codecSSID;
        subVendor = ssid >> 16;
        subDevice = ssid & 0xFFFF;
    } else {
        ssid = (subVendor << 16) | subDevice;
    }

    // A verified binding supplies speaker-board identity even when the host
    // HDA controller or codec publishes a different/spoofed subsystem ID.
    if (amp.firmwareSubsystemId) {
        ssid = amp.firmwareSubsystemId;
        subVendor = ssid >> 16;
        subDevice = ssid & 0xffff;
    }

    for (size_t q = 0; q < sizeof(kSsidQuirks) / sizeof(kSsidQuirks[0]); ++q) {
        if (!amp.firmwareSubsystemId && subVendor == kSsidQuirks[q].fromVendor && subDevice == kSsidQuirks[q].fromDevice) {
            subVendor = kSsidQuirks[q].toVendor;
            subDevice = kSsidQuirks[q].toDevice;
            ssid = (uint32_t(subVendor) << 16) | subDevice;
            CIRRUS_LOG("Applied SSID translation quirk: %04X:%04X -> %04X:%04X", kSsidQuirks[q].fromVendor, kSsidQuirks[q].fromDevice,
                       subVendor, subDevice);
            break;
        }
    }

    uint32_t explicitSSID = 0;
    if (parseBootArgument("-cirrusssid", &explicitSSID, sizeof(explicitSSID))) {
        ssid = explicitSSID;
        subVendor = ssid >> 16;
        subDevice = ssid & 0xFFFF;
        CIRRUS_LOG("Explicit firmware SSID override: 0x%08X", ssid);
    }

    // Unknown is not speaker 0 or 1. Specific protection coefficients require
    // measured/verified board identity or an explicit operator selection.
    uint32_t spkid = amp.firmwareSpeakerId;
    bool explicitSpeaker = parseBootArgument("-cirrusspkid", &spkid, sizeof(spkid));
    uint32_t channelSpeaker = spkid;
    const char* channelSpeakerArgument = amp.channel == core::AudioChannel::Right ? "-cirrusspkidr" : "-cirrusspkidl";
    if (parseBootArgument(channelSpeakerArgument, &channelSpeaker, sizeof(channelSpeaker))) {
        spkid = channelSpeaker;
        explicitSpeaker = true;
    }
    const char* speakerSource = explicitSpeaker ? "BOOT_ARGUMENT" : spkid != UINT32_MAX ? "BOARD_RESOURCE" : "UNKNOWN";
    if (explicitSpeaker && spkid > 255) {
        setProperty(propStatus, "INVALID_SPEAKER_ID_OVERRIDE");
        CIRRUS_ERR("Speaker ID override outside 0..255 on %s", amp.name);
        return;
    }
    CIRRUS_LOG("Firmware identity %s: SSID=0x%08X speaker=%u source=%s", amp.name, ssid, spkid, speakerSource);

    char speakerProperty[80];
    snprintf(speakerProperty, sizeof(speakerProperty), "Cirrus_Speaker_ID_%s", amp.name);
    setProperty(speakerProperty, uint64_t(spkid), 32);
    snprintf(speakerProperty, sizeof(speakerProperty), "Cirrus_Speaker_ID_Source_%s", amp.name);
    setProperty(speakerProperty, speakerSource);

    char propSSID[64];
    snprintf(propSSID, sizeof(propSSID), "Cirrus_SSID_%s", amp.name);
    setProperty(propSSID, (uint64_t)ssid, 32);

    // The codec may publish after the I2C provider. Zero means unavailable,
    // not an unsupported board, and must not permanently latch a boot fault.
    if (!ssid) {
        amp.firmwareIdentityPending = true;
        if (!amp.firmwareIdentityDeadlineMs)
            amp.firmwareIdentityDeadlineMs = playbackTimeMilliseconds() + 30000;
        setProperty(propStatus, "WAITING_FOR_CODEC_IDENTITY");
        return;
    }
    amp.firmwareIdentityPending = false;
    amp.firmwareIdentityDeadlineMs = 0;

    const FirmwareResource* foundRes =
        selectFirmwareResource(cs35l41Firmware, cs35l41FirmwareCount, uint16_t(subVendor), uint16_t(subDevice), spkid);

    if (!foundRes) {
        if (spkid == UINT32_MAX)
            CIRRUS_ERR(
                "Speaker identity unknown on %s; specific tuning requires verified speaker ID (-cirrusspkidl/-cirrusspkidr or -cirrusspkid)",
                amp.name);
        CIRRUS_ERR("No firmware resource matched Subsystem ID 0x%08X (vendor=0x%04X, device=0x%04X, spkid=%u) on %s", ssid, subVendor,
                   subDevice, spkid, amp.name);
        if (!gCirrusDebug) {
            CIRRUS_ERR("To dump full hardware profile, add '-cirrusdbg' to boot-args and reboot");
            CIRRUS_ERR("Report your hardware profile to: https://github.com/hoaug-tran/CirrusAudioFixup/issues");
        } else {
            IOLog(CIRRUS_LOG_PREFIX "=== CIRRUS AUDIO HARDWARE PROFILE DUMP ===\n");
            IOLog(CIRRUS_LOG_PREFIX "SSID: 0x%08X (Vendor: 0x%04X, Device: 0x%04X)\n", ssid, subVendor, subDevice);
            IOLog(CIRRUS_LOG_PREFIX "Amp: %s, I2C Address: 0x%02X\n", amp.name, amp.address);
            IOLog(CIRRUS_LOG_PREFIX "Hardware ID: DEVID=0x%08X REVID=0x%08X\n", amp.deviceId, amp.revisionId);
            IOLog(CIRRUS_LOG_PREFIX "Speaker ID: %u (explicit=%d)\n", spkid, explicitSpeaker ? 1 : 0);
            IOLog(CIRRUS_LOG_PREFIX "HDA Stream: Tag=%u Format=0x%04X\n", mHdaState.lastStreamTag, mHdaState.lastFormat);
            IOLog(CIRRUS_LOG_PREFIX "=== END HARDWARE PROFILE DUMP ===\n");
        }
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 0, ssid);
        OSString* statusStr = OSString::withCString(spkid == UINT32_MAX ? "SPEAKER_ID_REQUIRED" : "UNSUPPORTED_SSID_OR_SPEAKER");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfw == nullptr || foundRes->bin == nullptr) {
        CIRRUS_ERR("missing firmware binary or tuning files for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("FILE_NOT_FOUND");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfwSize == 0 || foundRes->binSize == 0) {
        CIRRUS_ERR("invalid firmware sizes for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if ((foundRes->wmfwSize % 4 != 0) || (foundRes->binSize % 4 != 0)) {
        CIRRUS_ERR("firmware files alignment must be 4-byte on ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_ALIGNMENT");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    if (foundRes->wmfwSize >= 4 && foundRes->wmfw[0] == 'W' && foundRes->wmfw[1] == 'M' && foundRes->wmfw[2] == 'F' &&
        foundRes->wmfw[3] == 'W') {
    } else {
        CIRRUS_ERR("wmfw magic header verification failed for ssid %08X", ssid);
        OSString* statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }

    char propFW[64];
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Source_%s", amp.name);
    OSString* srcStr = OSString::withCString("EmbeddedResource");
    if (srcStr) {
        setProperty(propFW, srcStr);
        srcStr->release();
    }

    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Type_%s", amp.name);
    OSString* typeStr = OSString::withCString(foundRes->isDummy ? "Dummy" : "Production");
    if (typeStr) {
        setProperty(propFW, typeStr);
        typeStr->release();
    }

    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Size_%s", amp.name);
    setProperty(propFW, (uint64_t)foundRes->wmfwSize, 32);

    bool isRight = amp.channel == cirrus::core::AudioChannel::Right;
    size_t chanBinSize = 0;
    const uint8_t* chanBin = selectChannelCoefficients(*foundRes, isRight, chanBinSize);
    if (!chanBin) {
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING);
        setProperty(propStatus, "INVALID_CHANNEL_TUNING");
        return;
    }
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_Size_%s", amp.name);
    setProperty(propFW, (uint64_t)chanBinSize, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_Channel_%s", amp.name);
    setProperty(propFW, isRight ? "RIGHT" : "LEFT");

    const uint8_t* tuningData = isRight ? foundRes->tuningRight : foundRes->tuningLeft;
    const size_t tuningSize = isRight ? foundRes->tuningRightSize : foundRes->tuningLeftSize;
    const char* tuningName = isRight ? foundRes->tuningRightName : foundRes->tuningLeftName;
    cirrus::devices::amplifiers::cs35l41::tuning::Parameters tuning{};
    if (!cirrus::devices::amplifiers::cs35l41::tuning::parse(tuningData, tuningSize, tuning)) {
        CIRRUS_ERR("invalid embedded gain tuning for %s; refusing this resource", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        setProperty(propStatus, "INVALID_GAIN_TUNING");
        return;
    }
    amp.tuningPcmGain = tuning.pcmGain;
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_PCM_Gain_%s", amp.name);
    setProperty(tuningProperty, uint64_t(amp.tuningPcmGain), 32);
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_Source_%s", amp.name);
    setProperty(tuningProperty, tuning.overridden ? (tuningName ? tuningName : "EMBEDDED_BINCFG") : "LINUX_DEFAULT");
    snprintf(tuningProperty, sizeof(tuningProperty), "Cirrus_Tuning_CRC32_%s", amp.name);
    setProperty(tuningProperty, uint64_t(tuningData ? calculateCrc32(tuningData, tuningSize) : 0), 32);

    uint32_t fwCrc = calculateCrc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculateCrc32(chanBin, chanBinSize);
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_CRC32_%s", amp.name);
    setProperty(propFW, (uint64_t)fwCrc, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_CRC32_%s", amp.name);
    setProperty(propFW, (uint64_t)binCrc, 32);

    uint32_t fwVersion = foundRes->wmfwSize >= 12 ? foundRes->wmfw[11] : 0;
    uint32_t binVersion = chanBinSize >= 12 ? WMFWParser::readLE32(chanBin + 8) : 0;

    char propFwVer[64];
    snprintf(propFwVer, sizeof(propFwVer), "Cirrus_WMFW_Container_Version_%s", amp.name);
    setProperty(propFwVer, (uint64_t)fwVersion, 32);

    char propBinVer[64];
    snprintf(propBinVer, sizeof(propBinVer), "Cirrus_BIN_Version_%s", amp.name);
    setProperty(propBinVer, (uint64_t)binVersion, 32);

    amp.wmfwData = foundRes->wmfw;
    amp.wmfwSize = foundRes->wmfwSize;
    amp.binData = chanBin;
    amp.binSize = chanBinSize;
    amp.firmwareValidated = false;

    CIRRUS_LOG("firmware matches found: %s (size: %lu, version: %08X, crc: %08X), tuning: %s (size: %lu, version: %08X, crc: %08X)",
               foundRes->fwName, amp.wmfwSize, fwVersion, fwCrc, foundRes->binName, amp.binSize, binVersion, binCrc);

    CIRRUS_LOG("tuning selection on %s: channel=%s spkid=%d bin=%s crc=0x%08X size=%lu (l0_crc=0x%08X r0_crc=0x%08X r0_present=%d)",
               amp.name, isRight ? "RIGHT(r0)" : "LEFT(l0)", spkid, (isRight && foundRes->binRight) ? "r0" : "l0", binCrc, chanBinSize,
               calculateCrc32(foundRes->bin, foundRes->binSize),
               foundRes->binRight ? calculateCrc32(foundRes->binRight, foundRes->binRightSize) : 0, foundRes->binRight ? 1 : 0);
    OSString* statusStr = OSString::withCString("READY");
    if (statusStr) {
        setProperty(propStatus, statusStr);
        statusStr->release();
    }
}

bool CS35L41Driver::bringUpDSP(AmplifierState& amp) {
    if (!amp.firmwareValidated || !amp.haloStateRegister || !amp.haloHeartbeatRegister)
        return false;
    auto failBoot = [&]() -> bool {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        if (!stopDSP(amp))
            amp.playbackFaulted = true;
        return false;
    };
    bool writesOk = true;
    auto checkedWrite = [&](uint32_t reg, uint32_t value) {
        if (writesOk)
            writesOk = writeRegister(amp, reg, value, TRACE_FIRMWARE);
    };
    char propName[64];
    OSString* statusStr = nullptr;

    uint32_t preCoreCtrl = 0, preClkCtrl = 0, preMbox = 0;
    uint32_t preSysId = 0, preSysVer = 0, preSysCore = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &preCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspClockControl, &preClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspMailbox2, &preMbox))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemId, &preSysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemVersion, &preSysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemCoreId, &preSysCore))
        return failBoot();
    CIRRUS_LOG("dsp pre-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X", amp.name,
               preCoreCtrl, preClkCtrl, preMbox, preSysId, preSysVer, preSysCore);

    static const uint32_t fsRateRegs[] = {
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx1Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx2Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx3Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx4Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx5Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx6Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx7Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Rx8Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx1Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx2Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx3Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx4Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx5Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx6Rate,
        cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx7Rate, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1Tx8Rate};
    bool fsErrataOk = true;
    for (size_t i = 0; i < sizeof(fsRateRegs) / sizeof(fsRateRegs[0]); ++i) {
        fsErrataOk &= writeRegister(amp, fsRateRegs[i], 0x00000001, TRACE_FIRMWARE);
    }
    CIRRUS_LOG("DSP FS errata on %s: %s", amp.name, fsErrataOk ? "applied" : "FAILED");
    if (!fsErrataOk)
        return failBoot();

    uint64_t mpuStart = mach_absolute_time();

    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuLockConfig, 0x5555);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuLockConfig, 0xAAAA);

    uint32_t unlockVal = 0xFFFFFFFF;
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXmAccess0, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYmAccess0, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuWndwAccess0, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXregAccess0, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYregAccess0, unlockVal);

    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXmAccess1, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYmAccess1, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuWndwAccess1, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXregAccess1, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYregAccess1, unlockVal);

    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXmAccess2, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYmAccess2, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuWndwAccess2, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXregAccess2, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYregAccess2, unlockVal);

    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXmAccess3, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYmAccess3, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuWndwAccess3, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuXregAccess3, unlockVal);
    checkedWrite(cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuYregAccess3, unlockVal);

    bool mpuLocked = writeRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1MpuLockConfig, 0);
    if (!writesOk || !mpuLocked)
        return failBoot();

    uint64_t mpuEnd = mach_absolute_time();
    (void)mpuStart;
    (void)mpuEnd;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t currentClk = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspClockControl, &currentClk))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)currentClk, 32);

    uint64_t resetStart = mach_absolute_time();

    if (!updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl,
                            cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset |
                                cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable,
                            cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset |
                                cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable) ||
        !updateRegisterBits(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl,
                            cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset, 0))
        return failBoot();

    uint64_t resetEnd = mach_absolute_time();
    uint64_t resetTimeUs = 0;
    absolutetime_to_nanoseconds(resetEnd - resetStart, &resetTimeUs);
    resetTimeUs /= 1000;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, resetTimeUs, 32);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t sysId = 0, sysVer = 0, sysCore = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemId, &sysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemVersion, &sysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemCoreId, &sysCore))
        return failBoot();

    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_ID_%s", amp.name);
    setProperty(propName, (uint64_t)sysId, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_VER_%s", amp.name);
    setProperty(propName, (uint64_t)sysVer, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_CORE_%s", amp.name);
    setProperty(propName, (uint64_t)sysCore, 32);

    uint32_t postCoreCtrl = 0, postClkCtrl = 0, postMbox = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &postCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspClockControl, &postClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspMailbox2, &postMbox))
        return failBoot();
    CIRRUS_LOG("dsp post-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X",
               amp.name, postCoreCtrl, postClkCtrl, postMbox, sysId, sysVer, sysCore);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_STATE_%s", amp.name);
    if (sysId == 0xFFFFFFFF && sysVer == 0xFFFFFFFF && sysCore == 0xFFFFFFFF) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_UNREACHABLE)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    } else if (sysId == 0x00000000 && sysVer == 0x00000000 && sysCore == 0x00000000) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_IN_RESET)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    }

    uint32_t mboxInit = 0;
    if (!readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspMailbox2, &mboxInit))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_RAW_INIT_%s", amp.name);
    setProperty(propName, (uint64_t)mboxInit, 32);

    const uint32_t HALO_STATE_REG = amp.haloStateRegister;
    const uint32_t HALO_STATE_CODE_RUN = 2;
    uint32_t haloState = 0;
    int haloTimeout = 15;
    while (haloTimeout > 0) {
        if (!readRegister(amp, HALO_STATE_REG, &haloState))
            return failBoot();
        if (haloState == HALO_STATE_CODE_RUN)
            break;
        IODelay(1000);
        haloTimeout--;
    }
    if (haloState == HALO_STATE_CODE_RUN) {
        CIRRUS_LOG("HALO firmware reached RUN state on %s (halo_state=0x%08X)", amp.name, haloState);

        if (!sendMailboxCommand(amp, cirrus::devices::amplifiers::cs35l41::registers::kCmdMailboxPause,
                                cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxPaused))
            return failBoot();
    } else {
        CIRRUS_ERR("HALO firmware did NOT reach RUN on %s (halo_state=0x%08X, expected 0x2). "
                   "DSP boot likely failed — check wmfw/coeff upload PASS count above.",
                   amp.name, haloState);
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HALO_STATE_%s", amp.name);
    setProperty(propName, (uint64_t)haloState, 32);

    const uint32_t HALO_HEARTBEAT_REG = amp.haloHeartbeatRegister;
    uint32_t hb0 = 0, hb1 = 0, hb2 = 0;
    bool hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb0);
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb1) && hbReadable;
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb2) && hbReadable;
    const bool heartbeatMoving = (hb0 != hb1) && (hb1 != hb2);
    const char* hbVerdict;
    if (!hbReadable) {
        hbVerdict = "READ_FAILED";
    } else if (heartbeatMoving) {
        hbVerdict = "CHANGING";
    } else {
        hbVerdict = "UNCHANGED_WHILE_PAUSED_NOT_A_BOOT_VERDICT";
    }
    CIRRUS_LOG("dsp heartbeat on %s: t0=0x%08X t1=0x%08X t2=0x%08X -> %s", amp.name, hb0, hb1, hb2, hbVerdict);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_%s", amp.name);
    setProperty(propName, (uint64_t)hb2, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_VERDICT_%s", amp.name);
    statusStr = OSString::withCString(hbVerdict);
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    if (haloState != HALO_STATE_CODE_RUN) {
        uint32_t xmVioSts = 0, xmVioAddr = 0;
        uint32_t ymVioSts = 0, ymVioAddr = 0;
        uint32_t pmVioSts = 0, pmVioAddr = 0;
        if (!readRegister(amp, 0x02BC3104, &xmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3100, &xmVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC310C, &ymVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3108, &ymVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC3114, &pmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3110, &pmVioAddr))
            return failBoot();

        bool actualMpuFault = ((xmVioSts & 0x007E0000) != 0) || ((ymVioSts & 0x007E0000) != 0) || ((pmVioSts & 0x007E0000) != 0);

        CIRRUS_ERR("MPU FAULT REPORT on %s: XM vio_sts=0x%08X vio_addr=0x%08X | "
                   "YM vio_sts=0x%08X vio_addr=0x%08X | PM vio_sts=0x%08X vio_addr=0x%08X",
                   amp.name, xmVioSts, xmVioAddr, ymVioSts, ymVioAddr, pmVioSts, pmVioAddr);
        if (actualMpuFault) {
            CIRRUS_ERR("  -> MPU BLOCKED an access. A non-zero vio_sts (bits 17-22) means the core was "
                       "denied memory access (MPU still locked / wrong region). This is the fault cause.");
        } else {
            CIRRUS_ERR("  -> No MPU violation latched. Core stall is NOT an MPU block; "
                       "suspect firmware image integrity (addressing/byte-order) or missing errata.");
        }

        uint32_t scratch[4] = {0};
        if (!readRegister(amp, 0x02B805C0, &scratch[0]))
            return failBoot();
        if (!readRegister(amp, 0x02B805C8, &scratch[1]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D0, &scratch[2]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D8, &scratch[3]))
            return failBoot();
        CIRRUS_ERR("DSP SCRATCH on %s: s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X "
                   "(non-zero => firmware ROM wrote a panic/abort code)",
                   amp.name, scratch[0], scratch[1], scratch[2], scratch[3]);

        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_XM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)xmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_YM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)ymVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_PM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)pmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_SCRATCH1_%s", amp.name);
        setProperty(propName, (uint64_t)scratch[0], 32);
        const uint32_t addresses[]{xmVioAddr, ymVioAddr, pmVioAddr, scratch[1], scratch[2], scratch[3]};
        const char* fields[]{"MPU_XM_ADDR", "MPU_YM_ADDR", "MPU_PM_ADDR", "SCRATCH2", "SCRATCH3", "SCRATCH4"};
        for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
            snprintf(propName, sizeof(propName), "Cirrus_DSP_%s_%s", fields[i], amp.name);
            setProperty(propName, uint64_t(addresses[i]), 32);
        }
    }

    if (!hbReadable || haloState != HALO_STATE_CODE_RUN)
        return failBoot();
    CIRRUS_LOG("dsp bringup complete for %s", amp.name);
    return true;
}

bool CS35L41Driver::verifyDSPAlive(AmplifierState& amp) {
    uint32_t coreCtrl = 0, sysId = 0, mbox = 0, haloState = 0;
    bool corePass = false, resetPass = false, sysIdPass = false, mboxPass = false, xmPass = false;

    bool readable = amp.haloStateRegister && amp.haloHeartbeatRegister &&
                    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1CcmCoreControl, &coreCtrl, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDsp1SystemId, &sysId, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::amplifiers::cs35l41::registers::kRegDspMailbox2, &mbox, TRACE_DUMP) &&
                    readRegister(amp, amp.haloStateRegister, &haloState, TRACE_DUMP);

    if (coreCtrl & cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreEnable)
        corePass = true;
    if ((coreCtrl & cirrus::devices::amplifiers::cs35l41::registers::kValHaloCoreReset) == 0)
        resetPass = true;
    if (sysId != 0x00000000 && sysId != 0xFFFFFFFF)
        sysIdPass = true;

    if (mbox == cirrus::devices::amplifiers::cs35l41::registers::kStatusMailboxPaused)
        mboxPass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xmPass = true;
    }

    uint32_t xmVio = 0, xmVioAddr = 0;
    uint32_t ymVio = 0, ymVioAddr = 0;
    uint32_t pmVio = 0, pmVioAddr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    readable = readRegister(amp, 0x2BC3104, &xmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ymVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ymVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP) && readable;

    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpuClean = ((xmVio & MPU_VIO_MASK) == 0 && (ymVio & MPU_VIO_MASK) == 0 && (pmVio & MPU_VIO_MASK) == 0);

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, sysid=%s(0x%08X), "
               "mailbox=%s(fw_status=%u), halo_state=0x%08X, xm_read=%s",
               amp.name, resetPass ? "ok" : "fail", corePass ? "ok" : "fail", sysIdPass ? "ok" : "fail", sysId, mboxPass ? "ok" : "fail",
               mbox, haloState, xmPass ? "ok" : "fail");
    CIRRUS_LOG("dsp verify MPU/scratch for %s: mpu=%s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X "
               "pm_vio=0x%08X@0x%08X scratch=[0x%08X 0x%08X 0x%08X 0x%08X]",
               amp.name, mpuClean ? "clean" : "FAULT", xmVio, xmVioAddr, ymVio, ymVioAddr, pmVio, pmVioAddr, sc1, sc2, sc3, sc4);

    uint32_t hb0 = 0, hb1 = 0;
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb0, TRACE_DUMP) && readable;
    IODelay(2000);
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb1, TRACE_DUMP) && readable;

    const uint32_t HALO_STATE_RUN = 2;
    bool runPass = (haloState == HALO_STATE_RUN);
    if (!runPass || !mpuClean || !mboxPass) {
        CIRRUS_ERR("dsp verdict for %s: halo=0x%08X heartbeat=0x%08X->0x%08X mpu=%s; DSP mode disabled", amp.name, haloState, hb0, hb1,
                   mpuClean ? "clean" : "FAULT");
    }

    return readable && corePass && resetPass && sysIdPass && mboxPass && xmPass && runPass && mpuClean;
}

bool CS35L41Driver::parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage) {
    uint8_t table[40 + 31 * 24] = {};
    outImage.algorithmCount = 0;
    if (!bulkRead(amp, 0x02800000, table, 40, TRACE_FIRMWARE))
        return false;
    uint32_t count = WMFWParser::readUnpacked32BE(table, 9);
    if (!count || count > 31)
        return false;
    size_t length = 40 + count * 24;
    for (size_t offset = 40; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > 252)
            chunk = 252;
        if (!bulkRead(amp, static_cast<uint32_t>(0x02800000 + offset), table + offset, chunk, TRACE_FIRMWARE))
            return false;
        offset += chunk;
    }
    return WMFWParser::parseAlgorithmTable(table, length, outImage);
}

void CS35L41Driver::uploadFirmware(AmplifierState& amp, const char* phaseArg) {
    (void)phaseArg;
    CIRRUS_LOG("starting firmware upload on %s", amp.name);

    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("no wmfw firmware data present on %s, skipping upload", amp.name);
        return;
    }

    FirmwareImage* image = (FirmwareImage*)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        return;
    }

    if (!WMFWParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    MappedImage* mappedImg = (MappedImage*)IOMalloc(sizeof(MappedImage));
    if (!mappedImg) {
        CIRRUS_ERR("failed to allocate memory for mapped image on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!WMFWMemoryMapper::mapFirmwareImage(*image, *mappedImg)) {
        CIRRUS_ERR("firmware mapping failed on %s", amp.name);
        IOFree(mappedImg, sizeof(MappedImage));
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    UploadSession session;
    DriverRegisterIO io(this, amp);
    if (WMFWUploadScheduler::run(amp.name, io, *mappedImg, session)) {
        CIRRUS_LOG("firmware upload complete on %s", amp.name);
    } else {
        CIRRUS_ERR("firmware upload failed on %s", amp.name);
    }

    IOFree(mappedImg, sizeof(MappedImage));
    IOFree(image, sizeof(FirmwareImage));
}
}
