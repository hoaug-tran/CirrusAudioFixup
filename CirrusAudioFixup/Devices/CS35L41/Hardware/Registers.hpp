//
// Registers.hpp
// CS35L41 register addresses, bitfields and diagnostic descriptions.
// Values apply to this silicon family. Keep their source and revision in
// review when changing them; a similarly named chip can use different bits.
// See LICENSE for distribution terms.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::devices::cs35l41::registers {

constexpr uint32_t kRegDeviceId = 0x00000000;
constexpr uint32_t kRegRevisionId = 0x00000004;
constexpr uint32_t kRegFabricationId = 0x00000008;
constexpr uint32_t kRegOtpId = 0x00000010;

constexpr uint32_t kRegPowerControl1 = 0x00002014;
constexpr uint32_t kRegPowerControl2 = 0x00002018;
constexpr uint32_t kRegPowerControl3 = 0x0000201C;
constexpr uint32_t kRegAmpOutputMute = 0x00002024;
constexpr uint32_t kRegPowerManagementStatus = 0x00002908;

// Linux include/sound/cs35l41.h: CS35L41_SFT_RESET is distinct from DEVID.
constexpr uint32_t kRegSoftwareReset = 0x00000020;
constexpr uint32_t kRegTestKeyControl = 0x00000040;

constexpr uint32_t kRegIrq1Status1 = 0x00010010;
constexpr uint32_t kRegIrq1Status2 = 0x00010014;
constexpr uint32_t kRegIrq1Status3 = 0x00010018;
constexpr uint32_t kRegIrq1Status4 = 0x0001001C;
constexpr uint32_t kRegIrq1RawStatus3 = 0x00010098;
constexpr uint32_t kRegIrq1Mask1 = 0x00010110;
constexpr uint32_t kRegIrq1Mask2 = 0x00010114;
constexpr uint32_t kRegIrq1Mask3 = 0x00010118;
constexpr uint32_t kRegIrq1Mask4 = 0x0001011C;
constexpr uint32_t kRegIrq2Status = 0x00010804;
constexpr uint32_t kRegIrq2Status1 = 0x00010810;
constexpr uint32_t kRegIrq2Mask1 = 0x00010910;
constexpr uint32_t kRegIrq2Mask2 = 0x00010914;
constexpr uint32_t kRegIrq2Mask3 = 0x00010918;
constexpr uint32_t kRegIrq2Mask4 = 0x0001091C;

constexpr uint32_t kRegPllClockControl = 0x00002C04;
constexpr uint32_t kRegDspClockControl = 0x00002C08;
constexpr uint32_t kRegGlobalClockControl = 0x00002C0C;

constexpr uint32_t kRegSerialPortRateControl = 0x00004804;
constexpr uint32_t kRegSerialPortFormat = 0x00004808;
constexpr uint32_t kRegSerialPortFrameTxSlot = 0x00004810;
constexpr uint32_t kRegSerialPortFrameRxSlot = 0x00004820;
constexpr uint32_t kRegSerialPortTxWordLength = 0x00004830;
constexpr uint32_t kRegSerialPortRxWordLength = 0x00004840;
constexpr uint32_t kRegDacPcm1Source = 0x00004C00;
constexpr uint32_t kRegAspTx1Source = 0x00004C20;
constexpr uint32_t kRegAspTx2Source = 0x00004C24;
constexpr uint32_t kRegAspTx3Source = 0x00004C28;
constexpr uint32_t kRegAspTx4Source = 0x00004C2C;
constexpr uint32_t kRegDsp1Rx1Source = 0x00004C40;
constexpr uint32_t kRegDsp1Rx2Source = 0x00004C44;
constexpr uint32_t kRegDsp1Rx3Source = 0x00004C48;
constexpr uint32_t kRegDsp1Rx4Source = 0x00004C4C;
constexpr uint32_t kRegDsp1Rx5Source = 0x00004C50;
constexpr uint32_t kRegDsp1Rx6Source = 0x00004C54;
constexpr uint32_t kRegSerialPortHighImpedanceControl = 0x0000480C;
constexpr uint32_t kRegSerialPortEnables = 0x00004800;

constexpr uint32_t kRegAmplifierDigitalVolumeControl = 0x00006000;
constexpr uint32_t kRegAmplifierGainControl = 0x00006C04;

// Linux HDA mute/unmute policy keeps the PCM high-pass enabled at unity gain.
// Clearing the volume word to zero also clears that filter; it is not an
// equivalent representation of the upstream unmuted state.
constexpr uint32_t kPlaybackDigitalVolume = 0x00008000;
// Linux HDA defaults: PCM code 17 (17.5 dB), PDM code 19 (19.5 dB).
// A board-specific tuning gain can differ; these defaults do not certify it.
constexpr uint32_t kPlaybackDspGain = (17U << 5) | 19U;
constexpr uint32_t kPlaybackBypassGain = (4U << 5) | 4U;

constexpr uint32_t kRegGpio1Control1 = 0x00011008;
constexpr uint32_t kRegGpio2Control1 = 0x0001100C;
constexpr uint32_t kRegGpioPadControl = 0x0000242C;

constexpr uint32_t kRegDsp1Rx1Rate = 0x02B80080;
constexpr uint32_t kRegDsp1Rx2Rate = 0x02B80088;
constexpr uint32_t kRegDsp1Rx3Rate = 0x02B80090;
constexpr uint32_t kRegDsp1Rx4Rate = 0x02B80098;
constexpr uint32_t kRegDsp1Rx5Rate = 0x02B800A0;
constexpr uint32_t kRegDsp1Rx6Rate = 0x02B800A8;
constexpr uint32_t kRegDsp1Rx7Rate = 0x02B800B0;
constexpr uint32_t kRegDsp1Rx8Rate = 0x02B800B8;
constexpr uint32_t kRegDsp1Tx1Rate = 0x02B80280;
constexpr uint32_t kRegDsp1Tx2Rate = 0x02B80288;
constexpr uint32_t kRegDsp1Tx3Rate = 0x02B80290;
constexpr uint32_t kRegDsp1Tx4Rate = 0x02B80298;
constexpr uint32_t kRegDsp1Tx5Rate = 0x02B802A0;
constexpr uint32_t kRegDsp1Tx6Rate = 0x02B802A8;
constexpr uint32_t kRegDsp1Tx7Rate = 0x02B802B0;
constexpr uint32_t kRegDsp1Tx8Rate = 0x02B802B8;

constexpr uint32_t kRegDsp1CcmCoreControl = 0x02BC1000;
constexpr uint32_t kRegDsp1CoreSoftReset = 0x02B80010;
constexpr uint32_t kRegDsp1SystemId = 0x025E0000;
constexpr uint32_t kRegDsp1SystemVersion = 0x025E0004;
constexpr uint32_t kRegDsp1SystemCoreId = 0x025E0008;

constexpr uint32_t kRegDspMailbox1 = 0x00013000;
constexpr uint32_t kRegDspMailbox2 = 0x00013004;
constexpr uint32_t kRegDspMbox3 = 0x00013008;
constexpr uint32_t kRegDspMbox4 = 0x0001300C;
constexpr uint32_t kRegDspVirtual1Mailbox1 = 0x00013020;

constexpr uint32_t kRegDsp1MpuLockConfig = 0x02BC3140;
constexpr uint32_t kRegDsp1MpuXmAccess0 = 0x02BC3000;
constexpr uint32_t kRegDsp1MpuYmAccess0 = 0x02BC3004;
constexpr uint32_t kRegDsp1MpuWndwAccess0 = 0x02BC3008;
constexpr uint32_t kRegDsp1MpuXregAccess0 = 0x02BC300C;
constexpr uint32_t kRegDsp1MpuYregAccess0 = 0x02BC3014;
constexpr uint32_t kRegDsp1MpuXmAccess1 = 0x02BC3018;
constexpr uint32_t kRegDsp1MpuYmAccess1 = 0x02BC301C;
constexpr uint32_t kRegDsp1MpuWndwAccess1 = 0x02BC3020;
constexpr uint32_t kRegDsp1MpuXregAccess1 = 0x02BC3024;
constexpr uint32_t kRegDsp1MpuYregAccess1 = 0x02BC302C;
constexpr uint32_t kRegDsp1MpuXmAccess2 = 0x02BC3030;
constexpr uint32_t kRegDsp1MpuYmAccess2 = 0x02BC3034;
constexpr uint32_t kRegDsp1MpuWndwAccess2 = 0x02BC3038;
constexpr uint32_t kRegDsp1MpuXregAccess2 = 0x02BC303C;
constexpr uint32_t kRegDsp1MpuYregAccess2 = 0x02BC3044;
constexpr uint32_t kRegDsp1MpuXmAccess3 = 0x02BC3048;
constexpr uint32_t kRegDsp1MpuYmAccess3 = 0x02BC304C;
constexpr uint32_t kRegDsp1MpuWndwAccess3 = 0x02BC3050;
constexpr uint32_t kRegDsp1MpuXregAccess3 = 0x02BC3054;
constexpr uint32_t kRegDsp1MpuYregAccess3 = 0x02BC305C;

constexpr uint32_t kValDeviceId = 0x00035A40;
constexpr uint32_t kValSoftwareReset = 0x5A000000;
constexpr uint32_t kValHaloCoreEnable = 0x00000001;
constexpr uint32_t kValHaloCoreReset = 0x00000200;

constexpr uint32_t kMaskOtpBootDone = 0x00000002;
constexpr uint32_t kMaskOtpBootError = 0x80000000;
constexpr uint32_t kMaskBoostEnable = 0x00000030;
constexpr uint32_t kMaskProtectionFault = 0x800281C0;

constexpr uint32_t kCmdMailboxResume = 2;
constexpr uint32_t kCmdMailboxPause = 1;
constexpr uint32_t kCmdMailboxSpeakerOutputEnable = 7;
constexpr uint32_t kCmdMailboxSpeakerOutputDisable = 8;

constexpr uint32_t kStatusMailboxRunning = 0;
constexpr uint32_t kStatusMailboxPaused = 1;
constexpr uint32_t kStatusMailboxReadyForReinitialization = 2;

constexpr uint8_t kI2cAddressLeft = 0x40;
constexpr uint8_t kI2cAddressRight = 0x41;

constexpr uint32_t kDeviceIdRegister = kRegDeviceId;
constexpr uint32_t kRevisionIdRegister = kRegRevisionId;
constexpr uint32_t kFabricationIdRegister = kRegFabricationId;
constexpr uint32_t kOtpIdRegister = kRegOtpId;
constexpr uint32_t kDeviceId = kValDeviceId;

constexpr uint32_t kPowerControl1 = kRegPowerControl1;
constexpr uint32_t kPowerControl2 = kRegPowerControl2;
constexpr uint32_t kPowerControl3 = kRegPowerControl3;
constexpr uint32_t kAmpOutputMute = kRegAmpOutputMute;
constexpr uint32_t kPowerManagementStatus = kRegPowerManagementStatus;

constexpr uint32_t kSoftwareReset = kRegSoftwareReset;
constexpr uint32_t kSoftwareResetValue = kValSoftwareReset;
constexpr uint32_t kTestKeyControl = kRegTestKeyControl;

constexpr uint32_t kIrq1Status1 = kRegIrq1Status1;
constexpr uint32_t kIrq1Status2 = kRegIrq1Status2;
constexpr uint32_t kIrq1Status3 = kRegIrq1Status3;
constexpr uint32_t kIrq1Status4 = kRegIrq1Status4;
constexpr uint32_t kIrq1RawStatus3 = kRegIrq1RawStatus3;
constexpr uint32_t kIrq1Status1Register = kRegIrq1Status1;
constexpr uint32_t kIrq1Mask1Register = kRegIrq1Mask1;
constexpr uint32_t kIrq1Mask1 = kRegIrq1Mask1;
constexpr uint32_t kIrq1Mask2 = kRegIrq1Mask2;
constexpr uint32_t kIrq1Mask3 = kRegIrq1Mask3;
constexpr uint32_t kIrq1Mask4 = kRegIrq1Mask4;

constexpr uint32_t kIrq2Status = kRegIrq2Status;
constexpr uint32_t kIrq2Status1Register = kRegIrq2Status1;
constexpr uint32_t kIrq2Mask1Register = kRegIrq2Mask1;
constexpr uint32_t kIrq2Mask1 = kRegIrq2Mask1;
constexpr uint32_t kIrq2Mask2 = kRegIrq2Mask2;
constexpr uint32_t kIrq2Mask3 = kRegIrq2Mask3;
constexpr uint32_t kIrq2Mask4 = kRegIrq2Mask4;

constexpr uint32_t kOtpBootDone = kMaskOtpBootDone;
constexpr uint32_t kOtpBootError = kMaskOtpBootError;
constexpr uint32_t kBoostEnableMask = kMaskBoostEnable;
constexpr uint32_t kProtectionMask = kMaskProtectionFault;

constexpr uint32_t kPllClockControl = kRegPllClockControl;
constexpr uint32_t kDspClockControl = kRegDspClockControl;
constexpr uint32_t kGlobalClockControl = kRegGlobalClockControl;

constexpr uint32_t kSerialPortRateControl = kRegSerialPortRateControl;
constexpr uint32_t kSerialPortFormat = kRegSerialPortFormat;
constexpr uint32_t kSerialPortFrameTxSlot = kRegSerialPortFrameTxSlot;
constexpr uint32_t kSerialPortFrameRxSlot = kRegSerialPortFrameRxSlot;
constexpr uint32_t kSerialPortTxWordLength = kRegSerialPortTxWordLength;
constexpr uint32_t kSerialPortRxWordLength = kRegSerialPortRxWordLength;
constexpr uint32_t kDacPcm1Source = kRegDacPcm1Source;
constexpr uint32_t kAspTx1Source = kRegAspTx1Source;
constexpr uint32_t kAspTx2Source = kRegAspTx2Source;
constexpr uint32_t kAspTx3Source = kRegAspTx3Source;
constexpr uint32_t kAspTx4Source = kRegAspTx4Source;
constexpr uint32_t kDsp1Rx1Source = kRegDsp1Rx1Source;
constexpr uint32_t kDsp1Rx2Source = kRegDsp1Rx2Source;
constexpr uint32_t kDsp1Rx3Source = kRegDsp1Rx3Source;
constexpr uint32_t kDsp1Rx4Source = kRegDsp1Rx4Source;
constexpr uint32_t kDsp1Rx5Source = kRegDsp1Rx5Source;
constexpr uint32_t kDsp1Rx6Source = kRegDsp1Rx6Source;
constexpr uint32_t kSerialPortHighImpedanceControl = kRegSerialPortHighImpedanceControl;
constexpr uint32_t kSerialPortEnables = kRegSerialPortEnables;

constexpr uint32_t kAmplifierDigitalVolumeControl = kRegAmplifierDigitalVolumeControl;
constexpr uint32_t kAmplifierGainControl = kRegAmplifierGainControl;

constexpr uint32_t kGpio1Control1 = kRegGpio1Control1;
constexpr uint32_t kGpio2Control1 = kRegGpio2Control1;
constexpr uint32_t kGpioPadControl = kRegGpioPadControl;

constexpr uint32_t kDsp1Rx1Rate = kRegDsp1Rx1Rate;
constexpr uint32_t kDsp1Rx2Rate = kRegDsp1Rx2Rate;
constexpr uint32_t kDsp1Rx3Rate = kRegDsp1Rx3Rate;
constexpr uint32_t kDsp1Rx4Rate = kRegDsp1Rx4Rate;
constexpr uint32_t kDsp1Rx5Rate = kRegDsp1Rx5Rate;
constexpr uint32_t kDsp1Rx6Rate = kRegDsp1Rx6Rate;
constexpr uint32_t kDsp1Rx7Rate = kRegDsp1Rx7Rate;
constexpr uint32_t kDsp1Rx8Rate = kRegDsp1Rx8Rate;
constexpr uint32_t kDsp1Tx1Rate = kRegDsp1Tx1Rate;
constexpr uint32_t kDsp1Tx2Rate = kRegDsp1Tx2Rate;
constexpr uint32_t kDsp1Tx3Rate = kRegDsp1Tx3Rate;
constexpr uint32_t kDsp1Tx4Rate = kRegDsp1Tx4Rate;
constexpr uint32_t kDsp1Tx5Rate = kRegDsp1Tx5Rate;
constexpr uint32_t kDsp1Tx6Rate = kRegDsp1Tx6Rate;
constexpr uint32_t kDsp1Tx7Rate = kRegDsp1Tx7Rate;
constexpr uint32_t kDsp1Tx8Rate = kRegDsp1Tx8Rate;

constexpr uint32_t kDsp1CcmCoreControl = kRegDsp1CcmCoreControl;
constexpr uint32_t kDsp1CoreSoftReset = kRegDsp1CoreSoftReset;
constexpr uint32_t kDsp1SystemId = kRegDsp1SystemId;
constexpr uint32_t kDsp1SystemVersion = kRegDsp1SystemVersion;
constexpr uint32_t kDsp1SystemCoreId = kRegDsp1SystemCoreId;

constexpr uint32_t kDspMailbox1 = kRegDspMailbox1;
constexpr uint32_t kDspMailbox2 = kRegDspMailbox2;
constexpr uint32_t kDspMbox1Register = kRegDspMailbox1;
constexpr uint32_t kDspMbox2Register = kRegDspMailbox2;
constexpr uint32_t kDspMbox3Register = kRegDspMbox3;
constexpr uint32_t kDspMbox4Register = kRegDspMbox4;
constexpr uint32_t kDspVirtual1Mailbox1 = kRegDspVirtual1Mailbox1;

constexpr uint32_t kMailboxCommandResume = kCmdMailboxResume;
constexpr uint32_t kMailboxCommandPause = kCmdMailboxPause;
constexpr uint32_t kMailboxCommandSpeakerOutputEnable = kCmdMailboxSpeakerOutputEnable;
constexpr uint32_t kMailboxCommandSpeakerOutputDisable = kCmdMailboxSpeakerOutputDisable;

constexpr uint32_t kMailboxStatusRunning = kStatusMailboxRunning;
constexpr uint32_t kMailboxStatusPaused = kStatusMailboxPaused;
constexpr uint32_t kMailboxStatusReadyForReinitialization = kStatusMailboxReadyForReinitialization;

constexpr uint32_t kHaloCoreEnable = kValHaloCoreEnable;
constexpr uint32_t kHaloCoreReset = kValHaloCoreReset;

constexpr uint32_t kDsp1MpuLockConfig = kRegDsp1MpuLockConfig;
constexpr uint32_t kDsp1MpuXmAccess0 = kRegDsp1MpuXmAccess0;
constexpr uint32_t kDsp1MpuYmAccess0 = kRegDsp1MpuYmAccess0;
constexpr uint32_t kDsp1MpuWndwAccess0 = kRegDsp1MpuWndwAccess0;
constexpr uint32_t kDsp1MpuXregAccess0 = kRegDsp1MpuXregAccess0;
constexpr uint32_t kDsp1MpuYregAccess0 = kRegDsp1MpuYregAccess0;
constexpr uint32_t kDsp1MpuXmAccess1 = kRegDsp1MpuXmAccess1;
constexpr uint32_t kDsp1MpuYmAccess1 = kRegDsp1MpuYmAccess1;
constexpr uint32_t kDsp1MpuWndwAccess1 = kRegDsp1MpuWndwAccess1;
constexpr uint32_t kDsp1MpuXregAccess1 = kRegDsp1MpuXregAccess1;
constexpr uint32_t kDsp1MpuYregAccess1 = kRegDsp1MpuYregAccess1;
constexpr uint32_t kDsp1MpuXmAccess2 = kRegDsp1MpuXmAccess2;
constexpr uint32_t kDsp1MpuYmAccess2 = kRegDsp1MpuYmAccess2;
constexpr uint32_t kDsp1MpuWndwAccess2 = kRegDsp1MpuWndwAccess2;
constexpr uint32_t kDsp1MpuXregAccess2 = kRegDsp1MpuXregAccess2;
constexpr uint32_t kDsp1MpuYregAccess2 = kRegDsp1MpuYregAccess2;
constexpr uint32_t kDsp1MpuXmAccess3 = kRegDsp1MpuXmAccess3;
constexpr uint32_t kDsp1MpuYmAccess3 = kRegDsp1MpuYmAccess3;
constexpr uint32_t kDsp1MpuWndwAccess3 = kRegDsp1MpuWndwAccess3;
constexpr uint32_t kDsp1MpuXregAccess3 = kRegDsp1MpuXregAccess3;
constexpr uint32_t kDsp1MpuYregAccess3 = kRegDsp1MpuYregAccess3;

}

struct RegisterDesc {
    uint32_t addr;
    const char* name;
    bool readable;
    bool volatileReg;
};

static const RegisterDesc cs35l41_reg_desc[] = {
    {0x00000, "CS35L41_DEVID", true, true},
    {0x00004, "CS35L41_REVID", true, true},
    {0x00008, "CS35L41_FABID", true, true},
    {0x0000C, "CS35L41_RELID", true, false},
    {0x00010, "CS35L41_OTPID", true, true},
    {0x00020, "CS35L41_SFT_RESET", true, true},
    {0x00040, "CS35L41_TEST_KEY_CTL", true, true},
    {0x00044, "CS35L41_USER_KEY_CTL", true, true},
    {0x00500, "CS35L41_OTP_CTRL0", true, false},
    {0x00508, "CS35L41_OTP_CTRL3", true, false},
    {0x0050C, "CS35L41_OTP_CTRL4", true, false},
    {0x00510, "CS35L41_OTP_CTRL5", true, false},
    {0x00514, "CS35L41_OTP_CTRL6", true, false},
    {0x00518, "CS35L41_OTP_CTRL7", true, false},
    {0x0051C, "CS35L41_OTP_CTRL8", true, false},
    {0x02014, "CS35L41_PWR_CTRL1", true, false},
    {0x02018, "CS35L41_PWR_CTRL2", true, false},
    {0x0201C, "CS35L41_PWR_CTRL3", true, false},
    {0x02020, "CS35L41_CTRL_OVRRIDE", true, false},
    {0x02024, "CS35L41_AMP_OUT_MUTE", true, false},
    {0x02030, "CS35L41_OTP_TRIM_36", true, false},
    {0x02034, "CS35L41_PROTECT_REL_ERR_IGN", true, false},
    {0x0208C, "CS35L41_OTP_TRIM_1", true, false},
    {0x02090, "CS35L41_OTP_TRIM_2", true, false},
    {0x0242C, "CS35L41_GPIO_PAD_CONTROL", true, false},
    {0x02438, "CS35L41_JTAG_CONTROL", true, false},
    {0x02900, "CS35L41_PWRMGT_CTL", true, true},
    {0x02904, "CS35L41_WAKESRC_CTL", true, true},
    {0x02908, "CS35L41_PWRMGT_STS", true, true},
    {0x02C04, "CS35L41_PLL_CLK_CTRL", true, false},
    {0x02C08, "CS35L41_DSP_CLK_CTRL", true, false},
    {0x02C0C, "CS35L41_GLOBAL_CLK_CTRL", true, false},
    {0x02C10, "CS35L41_DATA_FS_SEL", true, false},
    {0x02D10, "CS35L41_TST_FS_MON0", true, false},
    {0x0300C, "CS35L41_OTP_TRIM_4", true, false},
    {0x03010, "CS35L41_OTP_TRIM_3", true, false},
    {0x03018, "CS35L41_PLL_OVR", true, false},
    {0x03400, "CS35L41_MDSYNC_EN", true, false},
    {0x03408, "CS35L41_MDSYNC_TX_ID", true, false},
    {0x0340C, "CS35L41_MDSYNC_PWR_CTRL", true, false},
    {0x03410, "CS35L41_MDSYNC_DATA_TX", true, false},
    {0x03414, "CS35L41_MDSYNC_TX_STATUS", true, false},
    {0x0341C, "CS35L41_MDSYNC_DATA_RX", true, false},
    {0x03420, "CS35L41_MDSYNC_RX_STATUS", true, false},
    {0x03424, "CS35L41_MDSYNC_ERR_STATUS", true, false},
    {0x03528, "CS35L41_MDSYNC_SYNC_PTE2", true, false},
    {0x0352C, "CS35L41_MDSYNC_SYNC_PTE3", true, false},
    {0x0353C, "CS35L41_MDSYNC_SYNC_MSM_STATUS", true, false},
    {0x03800, "CS35L41_BSTCVRT_VCTRL1", true, false},
    {0x03804, "CS35L41_BSTCVRT_VCTRL2", true, false},
    {0x03808, "CS35L41_BSTCVRT_PEAK_CUR", true, false},
    {0x0380C, "CS35L41_BSTCVRT_SFT_RAMP", true, false},
    {0x03810, "CS35L41_BSTCVRT_COEFF", true, false},
    {0x03814, "CS35L41_BSTCVRT_SLOPE_LBST", true, false},
    {0x03818, "CS35L41_BSTCVRT_SW_FREQ", true, false},
    {0x0381C, "CS35L41_BSTCVRT_DCM_CTRL", true, false},
    {0x03820, "CS35L41_BSTCVRT_DCM_MODE_FORCE", true, false},
    {0x03830, "CS35L41_BSTCVRT_OVERVOLT_CTRL", true, false},
    {0x03900, "CS35L41_BST_TEST_DUTY", true, false},
    {0x0394C, "CS35L41_OTP_TRIM_5", true, false},
    {0x03950, "CS35L41_OTP_TRIM_6", true, false},
    {0x03954, "CS35L41_OTP_TRIM_7", true, false},
    {0x03958, "CS35L41_OTP_TRIM_8", true, false},
    {0x0395C, "CS35L41_OTP_TRIM_9", true, false},
    {0x04000, "CS35L41_VI_VOL_POL", true, false},
    {0x0400C, "CS35L41_OTP_TRIM_35", true, false},
    {0x0410C, "CS35L41_OTP_TRIM_34", true, false},
    {0x04160, "CS35L41_OTP_TRIM_11", true, false},
    {0x0416C, "CS35L41_OTP_TRIM_10", true, false},
    {0x04170, "CS35L41_OTP_TRIM_12", true, false},
    {0x04220, "CS35L41_DTEMP_WARN_THLD", true, false},
    {0x04224, "CS35L41_DTEMP_CFG", true, false},
    {0x04308, "CS35L41_DTEMP_EN", true, true},
    {0x04360, "CS35L41_OTP_TRIM_13", true, false},
    {0x04400, "CS35L41_VPVBST_FS_SEL", true, false},
    {0x04448, "CS35L41_OTP_TRIM_14", true, false},
    {0x0444C, "CS35L41_OTP_TRIM_15", true, false},
    {0x04800, "CS35L41_SP_ENABLES", true, false},
    {0x04804, "CS35L41_SP_RATE_CTRL", true, false},
    {0x04808, "CS35L41_SP_FORMAT", true, false},
    {0x0480C, "CS35L41_SP_HIZ_CTRL", true, false},
    {0x04810, "CS35L41_SP_FRAME_TX_SLOT", true, false},
    {0x04820, "CS35L41_SP_FRAME_RX_SLOT", true, false},
    {0x04830, "CS35L41_SP_TX_WL", true, false},
    {0x04840, "CS35L41_SP_RX_WL", true, false},
    {0x04C00, "CS35L41_DAC_PCM1_SRC", true, false},
    {0x04C20, "CS35L41_ASP_TX1_SRC", true, false},
    {0x04C24, "CS35L41_ASP_TX2_SRC", true, false},
    {0x04C28, "CS35L41_ASP_TX3_SRC", true, false},
    {0x04C2C, "CS35L41_ASP_TX4_SRC", true, false},
    {0x04C40, "CS35L41_DSP1_RX1_SRC", true, false},
    {0x04C44, "CS35L41_DSP1_RX2_SRC", true, false},
    {0x04C48, "CS35L41_DSP1_RX3_SRC", true, false},
    {0x04C4C, "CS35L41_DSP1_RX4_SRC", true, false},
    {0x04C50, "CS35L41_DSP1_RX5_SRC", true, false},
    {0x04C54, "CS35L41_DSP1_RX6_SRC", true, false},
    {0x04C58, "CS35L41_DSP1_RX7_SRC", true, false},
    {0x04C5C, "CS35L41_DSP1_RX8_SRC", true, false},
    {0x04C60, "CS35L41_NGATE1_SRC", true, false},
    {0x04C64, "CS35L41_NGATE2_SRC", true, false},
    {0x06000, "CS35L41_AMP_DIG_VOL_CTRL", true, false},
    {0x06404, "CS35L41_VPBR_CFG", true, false},
    {0x06408, "CS35L41_VBBR_CFG", true, false},
    {0x0640C, "CS35L41_VPBR_STATUS", true, false},
    {0x06410, "CS35L41_VBBR_STATUS", true, false},
    {0x06414, "CS35L41_OVERTEMP_CFG", true, false},
    {0x06418, "CS35L41_AMP_ERR_VOL", true, false},
    {0x06450, "CS35L41_VOL_STATUS_TO_DSP", true, false},
    {0x06800, "CS35L41_CLASSH_CFG", true, false},
    {0x06804, "CS35L41_WKFET_CFG", true, false},
    {0x06808, "CS35L41_NG_CFG", true, false},
    {0x06C04, "CS35L41_AMP_GAIN_CTRL", true, false},
    {0x06E30, "CS35L41_OTP_TRIM_16", true, false},
    {0x06E34, "CS35L41_OTP_TRIM_17", true, false},
    {0x06E38, "CS35L41_OTP_TRIM_18", true, false},
    {0x06E3C, "CS35L41_OTP_TRIM_19", true, false},
    {0x06E40, "CS35L41_OTP_TRIM_20", true, false},
    {0x06E44, "CS35L41_OTP_TRIM_21", true, false},
    {0x06E48, "CS35L41_OTP_TRIM_22", true, false},
    {0x06E4C, "CS35L41_OTP_TRIM_23", true, false},
    {0x06E50, "CS35L41_OTP_TRIM_24", true, false},
    {0x06E54, "CS35L41_OTP_TRIM_25", true, false},
    {0x06E58, "CS35L41_OTP_TRIM_26", true, false},
    {0x06E5C, "CS35L41_OTP_TRIM_27", true, false},
    {0x06E60, "CS35L41_OTP_TRIM_28", true, false},
    {0x06E64, "CS35L41_OTP_TRIM_29", true, false},
    {0x07068, "CS35L41_OTP_TRIM_33", true, false},
    {0x0706C, "CS35L41_DIGPWM_IOCTRL", true, false},
    {0x07400, "CS35L41_DAC_MSM_CFG", true, false},
    {0x07418, "CS35L41_OTP_TRIM_30", true, false},
    {0x0741C, "CS35L41_OTP_TRIM_31", true, false},
    {0x07434, "CS35L41_OTP_TRIM_32", true, false},
    {0x10000, "CS35L41_IRQ1_CFG", true, false},
    {0x10004, "CS35L41_IRQ1_STATUS", true, true},
    {0x10010, "CS35L41_IRQ1_STATUS1", true, true},
    {0x10014, "CS35L41_IRQ1_STATUS2", true, true},
    {0x10018, "CS35L41_IRQ1_STATUS3", true, true},
    {0x1001C, "CS35L41_IRQ1_STATUS4", true, true},
    {0x10090, "CS35L41_IRQ1_RAW_STATUS1", true, true},
    {0x10094, "CS35L41_IRQ1_RAW_STATUS2", true, true},
    {0x10098, "CS35L41_IRQ1_RAW_STATUS3", true, true},
    {0x1009C, "CS35L41_IRQ1_RAW_STATUS4", true, true},
    {0x10110, "CS35L41_IRQ1_MASK1", true, false},
    {0x10114, "CS35L41_IRQ1_MASK2", true, false},
    {0x10118, "CS35L41_IRQ1_MASK3", true, false},
    {0x1011C, "CS35L41_IRQ1_MASK4", true, false},
    {0x10190, "CS35L41_IRQ1_FRC1", true, false},
    {0x10194, "CS35L41_IRQ1_FRC2", true, false},
    {0x10198, "CS35L41_IRQ1_FRC3", true, false},
    {0x1019C, "CS35L41_IRQ1_FRC4", true, false},
    {0x10210, "CS35L41_IRQ1_EDGE1", true, false},
    {0x1021C, "CS35L41_IRQ1_EDGE4", true, false},
    {0x10290, "CS35L41_IRQ1_POL1", true, false},
    {0x10294, "CS35L41_IRQ1_POL2", true, false},
    {0x10298, "CS35L41_IRQ1_POL3", true, false},
    {0x1029C, "CS35L41_IRQ1_POL4", true, false},
    {0x10318, "CS35L41_IRQ1_DB3", true, false},
    {0x10800, "CS35L41_IRQ2_CFG", true, false},
    {0x10804, "CS35L41_IRQ2_STATUS", true, true},
    {0x10810, "CS35L41_IRQ2_STATUS1", true, true},
    {0x10814, "CS35L41_IRQ2_STATUS2", true, true},
    {0x10818, "CS35L41_IRQ2_STATUS3", true, true},
    {0x1081C, "CS35L41_IRQ2_STATUS4", true, true},
    {0x10890, "CS35L41_IRQ2_RAW_STATUS1", true, true},
    {0x10894, "CS35L41_IRQ2_RAW_STATUS2", true, true},
    {0x10898, "CS35L41_IRQ2_RAW_STATUS3", true, true},
    {0x1089C, "CS35L41_IRQ2_RAW_STATUS4", true, true},
    {0x10910, "CS35L41_IRQ2_MASK1", true, false},
    {0x10914, "CS35L41_IRQ2_MASK2", true, false},
    {0x10918, "CS35L41_IRQ2_MASK3", true, false},
    {0x1091C, "CS35L41_IRQ2_MASK4", true, false},
    {0x10990, "CS35L41_IRQ2_FRC1", true, false},
    {0x10994, "CS35L41_IRQ2_FRC2", true, false},
    {0x10998, "CS35L41_IRQ2_FRC3", true, false},
    {0x1099C, "CS35L41_IRQ2_FRC4", true, false},
    {0x10A10, "CS35L41_IRQ2_EDGE1", true, false},
    {0x10A1C, "CS35L41_IRQ2_EDGE4", true, false},
    {0x10A90, "CS35L41_IRQ2_POL1", true, false},
    {0x10A94, "CS35L41_IRQ2_POL2", true, false},
    {0x10A98, "CS35L41_IRQ2_POL3", true, false},
    {0x10A9C, "CS35L41_IRQ2_POL4", true, false},
    {0x10B18, "CS35L41_IRQ2_DB3", true, false},
    {0x11000, "CS35L41_GPIO_STATUS1", true, true},
    {0x11008, "CS35L41_GPIO1_CTRL1", true, false},
    {0x1100C, "CS35L41_GPIO2_CTRL1", true, false},
    {0x12000, "CS35L41_MIXER_NGATE_CFG", true, false},
    {0x12004, "CS35L41_MIXER_NGATE_CH1_CFG", true, false},
    {0x12008, "CS35L41_MIXER_NGATE_CH2_CFG", true, false},
    {0x14000, "CS35L41_CLOCK_DETECT_1", true, false},
    {0x17040, "CS35L41_DIE_STS1", true, false},
    {0x17044, "CS35L41_DIE_STS2", true, false},
    {0x17048, "CS35L41_TEMP_CAL1", true, false},
    {0x1704C, "CS35L41_TEMP_CAL2", true, false},
    {0x25C0800, "CS35L41_DSP1_TIMESTAMP_COUNT", true, false},
    {0x25E0000, "CS35L41_DSP1_SYS_ID", true, false},
    {0x25E0004, "CS35L41_DSP1_SYS_VERSION", true, false},
    {0x25E0008, "CS35L41_DSP1_SYS_CORE_ID", true, false},
    {0x25E000C, "CS35L41_DSP1_SYS_AHB_ADDR", true, false},
    {0x25E0010, "CS35L41_DSP1_SYS_XSRAM_SIZE", true, false},
    {0x25E0018, "CS35L41_DSP1_SYS_YSRAM_SIZE", true, false},
    {0x25E0020, "CS35L41_DSP1_SYS_PSRAM_SIZE", true, false},
    {0x25E0028, "CS35L41_DSP1_SYS_PM_BOOT_SIZE", true, false},
    {0x25E002C, "CS35L41_DSP1_SYS_FEATURES", true, false},
    {0x25E0030, "CS35L41_DSP1_SYS_FIR_FILTERS", true, false},
    {0x25E0034, "CS35L41_DSP1_SYS_LMS_FILTERS", true, false},
    {0x25E0038, "CS35L41_DSP1_SYS_XM_BANK_SIZE", true, false},
    {0x25E003C, "CS35L41_DSP1_SYS_YM_BANK_SIZE", true, false},
    {0x25E0040, "CS35L41_DSP1_SYS_PM_BANK_SIZE", true, false},
    {0x2B80080, "CS35L41_DSP1_RX1_RATE", true, false},
    {0x2B80088, "CS35L41_DSP1_RX2_RATE", true, false},
    {0x2B80090, "CS35L41_DSP1_RX3_RATE", true, false},
    {0x2B80098, "CS35L41_DSP1_RX4_RATE", true, false},
    {0x2B800A0, "CS35L41_DSP1_RX5_RATE", true, false},
    {0x2B800A8, "CS35L41_DSP1_RX6_RATE", true, false},
    {0x2B800B0, "CS35L41_DSP1_RX7_RATE", true, false},
    {0x2B800B8, "CS35L41_DSP1_RX8_RATE", true, false},
    {0x2B80280, "CS35L41_DSP1_TX1_RATE", true, false},
    {0x2B80288, "CS35L41_DSP1_TX2_RATE", true, false},
    {0x2B80290, "CS35L41_DSP1_TX3_RATE", true, false},
    {0x2B80298, "CS35L41_DSP1_TX4_RATE", true, false},
    {0x2B802A0, "CS35L41_DSP1_TX5_RATE", true, false},
    {0x2B802A8, "CS35L41_DSP1_TX6_RATE", true, false},
    {0x2B802B0, "CS35L41_DSP1_TX7_RATE", true, false},
    {0x2B802B8, "CS35L41_DSP1_TX8_RATE", true, false},
    {0x2B805C0, "CS35L41_DSP1_SCRATCH1", true, true},
    {0x2B805C8, "CS35L41_DSP1_SCRATCH2", true, true},
    {0x2B805D0, "CS35L41_DSP1_SCRATCH3", true, true},
    {0x2B805D8, "CS35L41_DSP1_SCRATCH4", true, true},
    {0x2BC1000, "CS35L41_DSP1_CCM_CORE_CTRL", true, false},
    {0x2B80010, "CS35L41_DSP1_CORE_SOFT_RESET", true, false},
    {0x2BC1008, "CS35L41_DSP1_CCM_CLK_OVERRIDE", true, false},
    {0x2BC2000, "CS35L41_DSP1_XM_MSTR_EN", true, false},
    {0x2BC2008, "CS35L41_DSP1_XM_CORE_PRI", true, false},
    {0x2BC2010, "CS35L41_DSP1_XM_AHB_PACK_PL_PRI", true, false},
    {0x2BC2018, "CS35L41_DSP1_XM_AHB_UP_PL_PRI", true, false},
    {0x2BC2020, "CS35L41_DSP1_XM_ACCEL_PL0_PRI", true, false},
    {0x2BC2078, "CS35L41_DSP1_XM_NPL0_PRI", true, false},
    {0x2BC20C0, "CS35L41_DSP1_YM_MSTR_EN", true, false},
    {0x2BC20C8, "CS35L41_DSP1_YM_CORE_PRI", true, false},
    {0x2BC20D0, "CS35L41_DSP1_YM_AHB_PACK_PL_PRI", true, false},
    {0x2BC20D8, "CS35L41_DSP1_YM_AHB_UP_PL_PRI", true, false},
    {0x2BC20E0, "CS35L41_DSP1_YM_ACCEL_PL0_PRI", true, false},
    {0x2BC2138, "CS35L41_DSP1_YM_NPL0_PRI", true, false},
    {0x2BC3000, "CS35L41_DSP1_MPU_XM_ACCESS0", true, false},
    {0x2BC3004, "CS35L41_DSP1_MPU_YM_ACCESS0", true, false},
    {0x2BC3008, "CS35L41_DSP1_MPU_WNDW_ACCESS0", true, false},
    {0x2BC300C, "CS35L41_DSP1_MPU_XREG_ACCESS0", true, false},
    {0x2BC3014, "CS35L41_DSP1_MPU_YREG_ACCESS0", true, false},
    {0x2BC3018, "CS35L41_DSP1_MPU_XM_ACCESS1", true, false},
    {0x2BC301C, "CS35L41_DSP1_MPU_YM_ACCESS1", true, false},
    {0x2BC3020, "CS35L41_DSP1_MPU_WNDW_ACCESS1", true, false},
    {0x2BC3024, "CS35L41_DSP1_MPU_XREG_ACCESS1", true, false},
    {0x2BC302C, "CS35L41_DSP1_MPU_YREG_ACCESS1", true, false},
    {0x2BC3030, "CS35L41_DSP1_MPU_XM_ACCESS2", true, false},
    {0x2BC3034, "CS35L41_DSP1_MPU_YM_ACCESS2", true, false},
    {0x2BC3038, "CS35L41_DSP1_MPU_WNDW_ACCESS2", true, false},
    {0x2BC303C, "CS35L41_DSP1_MPU_XREG_ACCESS2", true, false},
    {0x2BC3044, "CS35L41_DSP1_MPU_YREG_ACCESS2", true, false},
    {0x2BC3048, "CS35L41_DSP1_MPU_XM_ACCESS3", true, false},
    {0x2BC304C, "CS35L41_DSP1_MPU_YM_ACCESS3", true, false},
    {0x2BC3050, "CS35L41_DSP1_MPU_WNDW_ACCESS3", true, false},
    {0x2BC3054, "CS35L41_DSP1_MPU_XREG_ACCESS3", true, false},
    {0x2BC305C, "CS35L41_DSP1_MPU_YREG_ACCESS3", true, false},
    {0x2BC3100, "CS35L41_DSP1_MPU_XM_VIO_ADDR", true, false},
    {0x2BC3104, "CS35L41_DSP1_MPU_XM_VIO_STATUS", true, false},
    {0x2BC3108, "CS35L41_DSP1_MPU_YM_VIO_ADDR", true, false},
    {0x2BC310C, "CS35L41_DSP1_MPU_YM_VIO_STATUS", true, false},
    {0x2BC3110, "CS35L41_DSP1_MPU_PM_VIO_ADDR", true, false},
    {0x2BC3114, "CS35L41_DSP1_MPU_PM_VIO_STATUS", true, false},
    {0x2BC3140, "CS35L41_DSP1_MPU_LOCK_CONFIG", true, false},
    {0x2BC3180, "CS35L41_DSP1_MPU_WDT_RST_CTRL", true, false},
};
