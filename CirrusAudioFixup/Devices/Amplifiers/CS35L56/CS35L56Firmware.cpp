#include "Devices/Amplifiers/CS35L56/CS35L56Internal.hpp"
#include "Firmware/Calibration/EFICalibration.hpp"
#include "Firmware/Calibration/EFICalibrationSource.hpp"
#include "Firmware/WMFW/WMFWUploader.hpp"
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

namespace cirrus::devices::amplifiers::cs35l56 {
namespace {
// These objects exceed the kernel stack budget. All paths free partial allocation.
struct FirmwareWorkspace {
    FirmwareImage* image{static_cast<FirmwareImage*>(IOMallocData(sizeof(FirmwareImage)))};
    MappedImage* mapped{static_cast<MappedImage*>(IOMallocData(sizeof(MappedImage)))};
    UploadSession* session{static_cast<UploadSession*>(IOMallocData(sizeof(UploadSession)))};
    FirmwareWorkspace() = default;
    FirmwareWorkspace(const FirmwareWorkspace&) = delete;
    FirmwareWorkspace& operator=(const FirmwareWorkspace&) = delete;
    ~FirmwareWorkspace() {
        if (image)
            IOFreeData(image, sizeof(*image));
        if (mapped)
            IOFreeData(mapped, sizeof(*mapped));
        if (session)
            IOFreeData(session, sizeof(*session));
    }
    bool valid() const { return image && mapped && session; }
};
bool validMemory(const MappedImage& mapped) {
    for (uint32_t i = 0; i < mapped.regionCount; ++i) {
        const auto& r = mapped.regions[i];
        if (r.dspRegister == 0xFFFFFFFF)
            continue;
        if (!registers::dspMemoryRange(r.dspRegister, r.size))
            return false;
    }
    return true;
}
}
bool CS35L56Driver::applyCalibration(FirmwareImage& image) {
    firmware::EFICalibration calibration{};
    OSObject* property = firmware::copyEFICalibrationProperty();
    auto* data = OSDynamicCast(OSData, property);
    uint64_t uid = 0;
    const bool uidKnown = data && DSPControl::readSiliconUid(mContext.io, uid);
    const auto index = mContext.board.amplifiers[mContext.bindingIndex].channelIndex;
    const bool selected = uidKnown && firmware::EFICalibration::select(static_cast<const uint8_t*>(data->getBytesNoCopy()),
                                                                       data->getLength(), uid, index, calibration);
    OSSafeReleaseNULL(property);
    if (!selected) {
        publishState("CALIBRATION_UNAVAILABLE_OUTPUT_DISABLED");
        return false;
    }
    const char* names[]{"CAL_AMBIENT", "CAL_R", "CAL_STATUS", "CAL_CHECKSUM"};
    const uint32_t values[]{uint32_t(calibration.ambient), calibration.resistance, calibration.status, calibration.resistance + 1};
    uint32_t addresses[4]{};
    // Resolve the actual downloaded algorithm table, never a captured address.
    for (size_t i = 0; i < 4; ++i) {
        WMFWControlRef control{};
        if (!WMFWParser::findControl(&image, names[i], control) || control.algorithm->id != registers::kCalibrationAlgorithmId ||
            control.control->type != WMFW_ADSP2_YM || control.control->len != 4 ||
            !WMFWParser::resolveControl(image, control, addresses[i]))
            return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        uint32_t value = 0;
        if (!mContext.io.write(addresses[i], values[i]) || !mContext.io.read(addresses[i], &value)) {
            transferFailure(diagnostics::DIAG_DSP_BOOT, addresses[i]);
            return false;
        }
        if ((value & 0x00FFFFFF) != (values[i] & 0x00FFFFFF)) {
            fail(kIOReturnNotReady, diagnostics::DIAG_DSP_BOOT, addresses[i]);
            return false;
        }
    }
    return true;
}
IOReturn CS35L56Driver::loadFirmware() {
    using namespace registers;
    if (!writable() || mAmplifier.faulted || !mAmplifier.booted || !mResource || !mAmplifier.hardwareTouched ||
        mAmplifier.firmwareValidated)
        return kIOReturnNotReady;
    FirmwareWorkspace work;
    if (!work.valid())
        return fail(kIOReturnNoMemory, diagnostics::DIAG_FIRMWARE_MISSING);
    auto& image = *work.image;
    auto& mapped = *work.mapped;
    memset(static_cast<void*>(&image), 0, sizeof(image));
    memset(static_cast<void*>(&mapped), 0, sizeof(mapped));
    if (!WMFWParser::parseWMFW(mResource->wmfw, mResource->wmfwSize, &image))
        return fail(kIOReturnBadArgument, diagnostics::DIAG_FIRMWARE_MISSING);
    mHost.setDiagnosticStage(diagnostics::STAGE_FIRMWARE_UPLOAD);
    if (mAmplifier.firmwareMissing) {
        if (!WMFWMemoryMapper::mapFirmwareImage(image, mapped) || !validMemory(mapped))
            return fail(kIOReturnBadArgument, diagnostics::DIAG_FIRMWARE_MISSING);
        if (!DSPControl::shutdown(mContext.io, delayUs))
            return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegDspPowerState);
        mAmplifier.booted = false;
        if (!WMFWUploadScheduler::run(mHost.name(), mContext.io, mapped, *work.session))
            return fail(work.session->nativeError, diagnostics::DIAG_DSP_BOOT);
    }
    if (!DSPControl::readAlgorithms(mContext.io, image, kFirmwareId, kFirmwareVersion))
        return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegDspXMemory);
    size_t bytes = 0;
    const auto* coefficients =
        selectChannelCoefficients(*mResource, mContext.board.amplifiers[mContext.bindingIndex].channelIndex == 1, bytes);
    if (!WMFWParser::parseBIN(coefficients, bytes, &image) || !WMFWMemoryMapper::mapCoefficients(image, mapped) || !validMemory(mapped))
        return fail(kIOReturnBadArgument, diagnostics::DIAG_FIRMWARE_MISSING);
    if (!WMFWUploadScheduler::run(mHost.name(), mContext.io, mapped, *work.session))
        return fail(work.session->nativeError, diagnostics::DIAG_DSP_BOOT);
    if (mAmplifier.firmwareMissing && !DSPControl::resetSPI(mContext.io, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegDspVirtualMailbox2);
    mAmplifier.booted = true;
    if (!DSPControl::sendMailbox(mContext.io, kCommandPreventHibernate, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT);
    // Replay only the HDA/firmware configuration prescribed by Linux, muted.
    struct RegisterValue {
        uint32_t reg, value;
        bool verify{true};
    };
    constexpr RegisterValue configuration[]{
        {kRegAspEnables, 0},         {kRegUserMute, 1},           {kRegUserVolume, 0},         {kRegPosture, 0},
        {kRegSoundWireInput1, 0x18}, {kRegSoundWireInput2, 0x19}, {kRegSoundWireInput3, 0x29}, {kRegSoundWireInput4, 0x28},
        {kRegIrqMask18, 0x1F7DF0FF}, {kRegIrqMask20, 0xFFFFFFFF, false},
        {kRegAspControl1, 0x21},     {kRegAspControl2, 0x20200200},
        {kRegAspControl3, 3},        {kRegAspFrame1, 0x03020100}, {kRegAspFrame5, 0x00020100}, {kRegAspData5, 0x18},
        {kRegAspData1, 0x18},        {kRegAspTx1, 0x18},          {kRegAspTx2, 0x19},          {kRegAspTx3, 0x20},
        {kRegAspTx4, 0x28}};
    // Linux masks IRQ20 with an all-ones write; reserved bits need not read as ones.
    mHost.setDiagnosticStage(diagnostics::STAGE_ASP);
    for (const auto& entry : configuration) {
        uint32_t value = 0;
        if (!mContext.io.write(entry.reg, entry.value))
            return transferFailure(diagnostics::DIAG_ASP_CONFIG, entry.reg);
        if (!entry.verify)
            continue;
        if (!mContext.io.read(entry.reg, &value))
            return transferFailure(diagnostics::DIAG_ASP_CONFIG, entry.reg);
        if (value != entry.value)
            return fail(kIOReturnNotReady, diagnostics::DIAG_ASP_CONFIG, entry.reg, entry.value, value);
    }
    mHost.markDiagnosticSuccess(diagnostics::STAGE_ASP);
    mHost.setDiagnosticStage(diagnostics::STAGE_DSP_BOOT);
    if (!mContext.io.updateBits(kRegProtectionStatus, kFirmwareMissingMask, 0))
        return transferFailure(diagnostics::DIAG_DSP_BOOT, kRegProtectionStatus);
    mAmplifier.calibrated = applyCalibration(image);
    if (mAmplifier.faulted)
        return mAmplifier.latestFailure;
    if (!DSPControl::sendMailbox(mContext.io, kCommandAudioReinit, delayUs))
        return transferFailure(diagnostics::DIAG_DSP_BOOT);
    mAmplifier.firmwareMissing = false;
    mAmplifier.firmwareValidated = true;
    mHost.markDiagnosticSuccess(diagnostics::STAGE_DSP_BOOT);
    stopAt("firmware");
    return kIOReturnSuccess;
}
}
