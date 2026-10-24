#pragma once
#include <stdint.h>

namespace cirrus::devices::amplifiers::cs35l56::registers {
constexpr uint32_t kRegDeviceId = 0x00000000;
constexpr uint32_t kRegRevisionId = 0x00000004;
constexpr uint32_t kRegReleaseId = 0x0000000C;
constexpr uint32_t kRegOtpId = 0x00000010;
constexpr uint32_t kDeviceIdMask = 0x00FFFFFF;
constexpr uint32_t kValDeviceId = 0x00035A56;
constexpr uint32_t kRevisionIdMask = 0x000000FF;
constexpr uint32_t kRevisionB0 = 0x000000B0;
constexpr uint32_t kRegDspRawMailbox1 = 0x00011000;
constexpr uint32_t kRegDspVirtualMailbox1 = 0x00011020;
constexpr uint32_t kRegDspVirtualMailbox2 = 0x00011024;
constexpr uint32_t kRegDspHaloState = 0x028021E0;
constexpr uint32_t kRegDspPowerState = 0x02804308;
constexpr uint32_t kRegDspXMemory = 0x02800000;
constexpr uint32_t kCommandSystemReset = 0x02000007;
constexpr uint32_t kCommandPing = 0x0A000000;
constexpr uint32_t kCommandShutdown = 0x02000005;
constexpr uint32_t kSPIResetToPortReadyUs = 4700;
// Linux 7be4bc52fd9e cs35l56.h: B0 register and firmware protocol.
constexpr uint32_t kRegIrqStatus = 0x0000E004;
constexpr uint32_t kRegIrqMask20 = 0x0000E0DC;
constexpr uint32_t kRegIrqMask18 = 0x0000E0D4;
constexpr uint32_t kRegIrqFault1 = 0x0000E010;
constexpr uint32_t kRegIrqFault8 = 0x0000E02C;
constexpr uint32_t kRegOtpMemory53 = 0x000300D4;
constexpr uint32_t kRegRestricted = 0x000190F0;
constexpr uint32_t kRegFirmwareVersion = 0x02800010;
constexpr uint32_t kRegProtectionStatus = 0x034000D8;
constexpr uint32_t kRegUserMute = 0x03400024;
constexpr uint32_t kRegUserVolume = 0x0340002C;
constexpr uint32_t kRegPosture = 0x03400094;
constexpr uint32_t kRegTransducerState = 0x03400150;
constexpr uint32_t kRegAspEnables = 0x00004800;
constexpr uint32_t kRegAspControl1 = 0x00004804;
constexpr uint32_t kRegAspControl2 = 0x00004808;
constexpr uint32_t kRegAspControl3 = 0x0000480C;
constexpr uint32_t kRegAspFrame1 = 0x00004810;
constexpr uint32_t kRegAspFrame5 = 0x00004820;
constexpr uint32_t kRegAspData1 = 0x00004830;
constexpr uint32_t kRegAspData5 = 0x00004840;
constexpr uint32_t kRegAspTx1 = 0x00004C20;
constexpr uint32_t kRegAspTx2 = 0x00004C24;
constexpr uint32_t kRegAspTx3 = 0x00004C28;
constexpr uint32_t kRegAspTx4 = 0x00004C2C;
constexpr uint32_t kRegSoundWireInput1 = 0x00004C70;
constexpr uint32_t kRegSoundWireInput2 = 0x00004C74;
constexpr uint32_t kRegSoundWireInput3 = 0x00004C78;
constexpr uint32_t kRegSoundWireInput4 = 0x00004C7C;
constexpr uint32_t kFirmwareId = 0x001A00D6;
constexpr uint32_t kFirmwareVersion = 0x00030B10;
constexpr uint32_t kFirmwareMissingMask = 1;
constexpr uint32_t kAspReceiveMask = 0x00030000;
constexpr uint32_t kCommandPreventHibernate = 0x02000003;
constexpr uint32_t kCommandAudioPlay = 0x0B000001;
constexpr uint32_t kCommandAudioPause = 0x0B000002;
constexpr uint32_t kCommandAudioReinit = 0x0B000003;
struct MemoryRange {
    uint32_t first, end;
};
inline constexpr MemoryRange kDspMemoryRanges[]{
    {0x02000000, 0x02006000}, {0x02800000, 0x02808000}, {0x02C00000, 0x02C047F4}, {0x03400000, 0x03405FF8}, {0x03800000, 0x03804FEC}};
constexpr bool dspMemoryRange(uint32_t reg, uint32_t bytes) {
    if (!bytes || (reg & 3) || (bytes & 3))
        return false;
    for (const auto& range : kDspMemoryRanges)
        if (reg >= range.first && uint64_t(reg) + bytes <= range.end)
            return true;
    return false;
}
constexpr bool dspMemoryRegister(uint32_t reg) { return dspMemoryRange(reg, 4); }
constexpr uint32_t kControlPortReadyUs = 2200;
constexpr uint32_t kWakeHoldTimeUs = 1000;
constexpr uint32_t kHaloStatePollUs = 1000;
constexpr uint32_t kHaloStateTimeoutUs = 250000;
constexpr uint32_t kMailboxPollUs = 250;
constexpr uint32_t kMailboxTimeoutUs = 5000;
constexpr uint32_t kPS0PollUs = 500;
constexpr uint32_t kPS0TimeoutUs = 50000;
constexpr uint32_t kPS3PollUs = 500;
constexpr uint32_t kPS3TimeoutUs = 300000;
constexpr uint32_t kRestrictedMask = 7;
constexpr uint32_t kProtectionFaultMask = 0x80000000;
constexpr uint32_t kCalibrationAlgorithmId = 0x0009F210;
constexpr uint32_t kLastRegister = 0x03804FE8;
}
