from pathlib import Path
import re
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
REG_HPP = ROOT / 'CirrusAudioFixup/Devices/CS35L41/Hardware/Registers.hpp'
content = REG_HPP.read_text(encoding='utf-8')

constexprs = dict(re.findall(r'(?:inline\s+)?constexpr\s+(?:uint8_t|uint16_t|uint32_t)\s+([A-Za-z0-9_]+)\s*=\s*([^;]+);', content))

if not constexprs:
    print("ERROR: No constexpr registers found in Registers.hpp", file=sys.stderr)
    sys.exit(1)

LEGACY_MACROS = [
    ('CS35L41_SW_RESET', '0x00000020'),
    ('CS35L41_SW_RESET_VAL', '0x5A000000'),
    ('CS35L41_TEST_KEY_CTL', '0x00000040'),
    ('CS35L41_IRQ1_STATUS4', '0x0001001C'),
    ('CS35L41_OTP_BOOT_DONE', '0x00000002'),
    ('CS35L41_IRQ1_STATUS3', '0x00010018'),
    ('CS35L41_OTP_BOOT_ERR', '0x80000000'),
    ('CS35L41_IRQ1_RAW_STATUS3', '0x00010098'),
    ('CS35L41_BST_EN_MASK', '0x00000030'),
    ('CS35L41_PROTECTION_MASK', '0x800281C0'),
    ('CS35L41_IRQ1_MASK1', '0x00010110'),
    ('CS35L41_IRQ1_MASK2', '0x00010114'),
    ('CS35L41_IRQ1_MASK3', '0x00010118'),
    ('CS35L41_IRQ1_MASK4', '0x0001011C'),
    ('CS35L41_IRQ2_MASK1', '0x00010910'),
    ('CS35L41_IRQ2_MASK2', '0x00010914'),
    ('CS35L41_IRQ2_MASK3', '0x00010918'),
    ('CS35L41_IRQ2_MASK4', '0x0001091C'),
    ('CS35L41_PLL_CLK_CTRL', '0x00002C04'),
    ('CS35L41_DSP_CLK_CTRL', '0x00002C08'),
    ('CS35L41_GLOBAL_CLK_CTRL', '0x00002C0C'),
    ('CS35L41_SP_RATE_CTRL', '0x00004804'),
    ('CS35L41_SP_FORMAT', '0x00004808'),
    ('CS35L41_SP_FRAME_TX_SLOT', '0x00004810'),
    ('CS35L41_SP_FRAME_RX_SLOT', '0x00004820'),
    ('CS35L41_SP_TX_WL', '0x00004830'),
    ('CS35L41_SP_RX_WL', '0x00004840'),
    ('CS35L41_DAC_PCM1_SRC', '0x00004C00'),
    ('CS35L41_ASP_TX1_SRC', '0x00004C20'),
    ('CS35L41_ASP_TX2_SRC', '0x00004C24'),
    ('CS35L41_ASP_TX3_SRC', '0x00004C28'),
    ('CS35L41_ASP_TX4_SRC', '0x00004C2C'),
    ('CS35L41_DSP1_RX1_SRC', '0x00004C40'),
    ('CS35L41_DSP1_RX2_SRC', '0x00004C44'),
    ('CS35L41_DSP1_RX3_SRC', '0x00004C48'),
    ('CS35L41_DSP1_RX4_SRC', '0x00004C4C'),
    ('CS35L41_DSP1_RX5_SRC', '0x00004C50'),
    ('CS35L41_DSP1_RX6_SRC', '0x00004C54'),
    ('CS35L41_SP_HIZ_CTRL', '0x0000480C'),
    ('CS35L41_SP_ENABLES', '0x00004800'),
    ('CS35L41_AMP_DIG_VOL_CTRL', '0x00006000'),
    ('CS35L41_AMP_GAIN_CTRL', '0x00006C04'),
    ('CS35L41_GPIO1_CTRL1', '0x00011008'),
    ('CS35L41_GPIO2_CTRL1', '0x0001100C'),
    ('CS35L41_GPIO_PAD_CONTROL', '0x0000242C'),
    ('CS35L41_DSP1_RX1_RATE', '0x02B80080'),
    ('CS35L41_DSP1_RX2_RATE', '0x02B80088'),
    ('CS35L41_DSP1_RX3_RATE', '0x02B80090'),
    ('CS35L41_DSP1_RX4_RATE', '0x02B80098'),
    ('CS35L41_DSP1_RX5_RATE', '0x02B800A0'),
    ('CS35L41_DSP1_RX6_RATE', '0x02B800A8'),
    ('CS35L41_DSP1_RX7_RATE', '0x02B800B0'),
    ('CS35L41_DSP1_RX8_RATE', '0x02B800B8'),
    ('CS35L41_DSP1_TX1_RATE', '0x02B80280'),
    ('CS35L41_DSP1_TX2_RATE', '0x02B80288'),
    ('CS35L41_DSP1_TX3_RATE', '0x02B80290'),
    ('CS35L41_DSP1_TX4_RATE', '0x02B80298'),
    ('CS35L41_DSP1_TX5_RATE', '0x02B802A0'),
    ('CS35L41_DSP1_TX6_RATE', '0x02B802A8'),
    ('CS35L41_DSP1_TX7_RATE', '0x02B802B0'),
    ('CS35L41_DSP1_TX8_RATE', '0x02B802B8'),
    ('CS35L41_DSP1_CCM_CORE_CTRL', '0x02BC1000'),
    ('CS35L41_DSP1_CORE_SOFT_RESET', '0x02B80010'),
    ('CS35L41_DSP1_SYS_ID', '0x025E0000'),
    ('CS35L41_DSP1_SYS_VERSION', '0x025E0004'),
    ('CS35L41_DSP1_SYS_CORE_ID', '0x025E0008'),
    ('CS35L41_DSP_MBOX_1', '0x00013000'),
    ('CS35L41_DSP_MBOX_2', '0x00013004'),
    ('CS35L41_DSP_VIRT1_MBOX_1', '0x00013020'),
    ('CSPL_MBOX_CMD_RESUME', '2'),
    ('CSPL_MBOX_CMD_PAUSE', '1'),
    ('CSPL_MBOX_CMD_SPK_OUT_ENABLE', '7'),
    ('CSPL_MBOX_STS_RUNNING', '0'),
    ('CSPL_MBOX_STS_PAUSED', '1'),
    ('CSPL_MBOX_STS_RDY_FOR_REINIT', '2'),
    ('CS35L41_IRQ1_STATUS1', '0x00010010'),
    ('CS35L41_IRQ1_STATUS2', '0x00010014'),
    ('CS35L41_IRQ2_STATUS', '0x00010804'),
    ('HALO_CORE_EN', '0x00000001'),
    ('HALO_CORE_RESET', '0x00000200'),
    ('CS35L41_DSP1_MPU_LOCK_CONFIG', '0x02BC3140'),
    ('CS35L41_DSP1_MPU_XM_ACCESS0', '0x02BC3000'),
    ('CS35L41_DSP1_MPU_YM_ACCESS0', '0x02BC3004'),
    ('CS35L41_DSP1_MPU_WNDW_ACCESS0', '0x02BC3008'),
    ('CS35L41_DSP1_MPU_XREG_ACCESS0', '0x02BC300C'),
    ('CS35L41_DSP1_MPU_YREG_ACCESS0', '0x02BC3014'),
    ('CS35L41_DSP1_MPU_XM_ACCESS1', '0x02BC3018'),
    ('CS35L41_DSP1_MPU_YM_ACCESS1', '0x02BC301C'),
    ('CS35L41_DSP1_MPU_WNDW_ACCESS1', '0x02BC3020'),
    ('CS35L41_DSP1_MPU_XREG_ACCESS1', '0x02BC3024'),
    ('CS35L41_DSP1_MPU_YREG_ACCESS1', '0x02BC302C'),
    ('CS35L41_DSP1_MPU_XM_ACCESS2', '0x02BC3030'),
    ('CS35L41_DSP1_MPU_YM_ACCESS2', '0x02BC3034'),
    ('CS35L41_DSP1_MPU_WNDW_ACCESS2', '0x02BC3038'),
    ('CS35L41_DSP1_MPU_XREG_ACCESS2', '0x02BC303C'),
    ('CS35L41_DSP1_MPU_YREG_ACCESS2', '0x02BC3044'),
    ('CS35L41_DSP1_MPU_XM_ACCESS3', '0x02BC3048'),
    ('CS35L41_DSP1_MPU_YM_ACCESS3', '0x02BC304C'),
    ('CS35L41_DSP1_MPU_WNDW_ACCESS3', '0x02BC3050'),
    ('CS35L41_DSP1_MPU_XREG_ACCESS3', '0x02BC3054'),
    ('CS35L41_DSP1_MPU_YREG_ACCESS3', '0x02BC305C'),
]

SPECIAL_NAMES = {
    'CS35L41_I2C_ADDR_LEFT': 'kI2cAddressLeft',
    'CS35L41_I2C_ADDR_RIGHT': 'kI2cAddressRight',
    'CS35L41_DEVICE_ID': 'kDeviceId',
    'CS35L41_DEVID_REG': 'kDeviceIdRegister',
    'CS35L41_REVID_REG': 'kRevisionIdRegister',
    'CS35L41_FABID_REG': 'kFabricationIdRegister',
    'CS35L41_OTPID_REG': 'kOtpIdRegister',
    'CS35L41_PWR_CTRL1_REG': 'kPowerControl1',
    'CS35L41_PWR_CTRL2_REG': 'kPowerControl2',
    'CS35L41_PWR_CTRL3_REG': 'kPowerControl3',
    'CS35L41_AMP_OUT_MUTE_REG': 'kAmpOutputMute',
    'CS35L41_PWRMGT_STS_REG': 'kPowerManagementStatus',
    'CS35L41_IRQ1_STATUS1_REG': 'kIrq1Status1Register',
    'CS35L41_IRQ2_STATUS1_REG': 'kIrq2Status1Register',
    'CS35L41_IRQ1_MASK1_REG': 'kIrq1Mask1Register',
    'CS35L41_IRQ2_MASK1_REG': 'kIrq2Mask1Register',
    'CS35L41_DSP_MBOX_1_REG': 'kDspMbox1Register',
    'CS35L41_DSP_MBOX_2_REG': 'kDspMbox2Register',
    'CS35L41_DSP_MBOX_3_REG': 'kDspMbox3Register',
    'CS35L41_DSP_MBOX_4_REG': 'kDspMbox4Register',
    'CS35L41_SW_RESET': 'kSoftwareReset',
    'CS35L41_SW_RESET_VAL': 'kSoftwareResetValue',
    'CS35L41_TEST_KEY_CTL': 'kTestKeyControl',
    'CS35L41_IRQ1_STATUS4': 'kIrq1Status4',
    'CS35L41_OTP_BOOT_DONE': 'kOtpBootDone',
    'CS35L41_IRQ1_STATUS3': 'kIrq1Status3',
    'CS35L41_OTP_BOOT_ERR': 'kOtpBootError',
    'CS35L41_IRQ1_RAW_STATUS3': 'kIrq1RawStatus3',
    'CS35L41_BST_EN_MASK': 'kBoostEnableMask',
    'CS35L41_PROTECTION_MASK': 'kProtectionMask',
    'CS35L41_IRQ1_MASK1': 'kIrq1Mask1',
    'CS35L41_IRQ1_MASK2': 'kIrq1Mask2',
    'CS35L41_IRQ1_MASK3': 'kIrq1Mask3',
    'CS35L41_IRQ1_MASK4': 'kIrq1Mask4',
    'CS35L41_IRQ2_MASK1': 'kIrq2Mask1',
    'CS35L41_IRQ2_MASK2': 'kIrq2Mask2',
    'CS35L41_IRQ2_MASK3': 'kIrq2Mask3',
    'CS35L41_IRQ2_MASK4': 'kIrq2Mask4',
    'CS35L41_PLL_CLK_CTRL': 'kPllClockControl',
    'CS35L41_DSP_CLK_CTRL': 'kDspClockControl',
    'CS35L41_GLOBAL_CLK_CTRL': 'kGlobalClockControl',
    'CS35L41_SP_RATE_CTRL': 'kSerialPortRateControl',
    'CS35L41_SP_FORMAT': 'kSerialPortFormat',
    'CS35L41_SP_FRAME_TX_SLOT': 'kSerialPortFrameTxSlot',
    'CS35L41_SP_FRAME_RX_SLOT': 'kSerialPortFrameRxSlot',
    'CS35L41_SP_TX_WL': 'kSerialPortTxWordLength',
    'CS35L41_SP_RX_WL': 'kSerialPortRxWordLength',
    'CS35L41_SP_HIZ_CTRL': 'kSerialPortHighImpedanceControl',
    'CS35L41_SP_ENABLES': 'kSerialPortEnables',
    'CS35L41_DAC_PCM1_SRC': 'kDacPcm1Source',
    'CS35L41_ASP_TX1_SRC': 'kAspTx1Source',
    'CS35L41_ASP_TX2_SRC': 'kAspTx2Source',
    'CS35L41_ASP_TX3_SRC': 'kAspTx3Source',
    'CS35L41_ASP_TX4_SRC': 'kAspTx4Source',
    'CS35L41_DSP1_RX1_SRC': 'kDsp1Rx1Source',
    'CS35L41_DSP1_RX2_SRC': 'kDsp1Rx2Source',
    'CS35L41_DSP1_RX3_SRC': 'kDsp1Rx3Source',
    'CS35L41_DSP1_RX4_SRC': 'kDsp1Rx4Source',
    'CS35L41_DSP1_RX5_SRC': 'kDsp1Rx5Source',
    'CS35L41_DSP1_RX6_SRC': 'kDsp1Rx6Source',
    'CS35L41_AMP_DIG_VOL_CTRL': 'kAmplifierDigitalVolumeControl',
    'CS35L41_AMP_GAIN_CTRL': 'kAmplifierGainControl',
    'CS35L41_GPIO1_CTRL1': 'kGpio1Control1',
    'CS35L41_GPIO2_CTRL1': 'kGpio2Control1',
    'CS35L41_GPIO_PAD_CONTROL': 'kGpioPadControl',
    'CS35L41_DSP1_RX1_RATE': 'kDsp1Rx1Rate',
    'CS35L41_DSP1_RX2_RATE': 'kDsp1Rx2Rate',
    'CS35L41_DSP1_RX3_RATE': 'kDsp1Rx3Rate',
    'CS35L41_DSP1_RX4_RATE': 'kDsp1Rx4Rate',
    'CS35L41_DSP1_RX5_RATE': 'kDsp1Rx5Rate',
    'CS35L41_DSP1_RX6_RATE': 'kDsp1Rx6Rate',
    'CS35L41_DSP1_RX7_RATE': 'kDsp1Rx7Rate',
    'CS35L41_DSP1_RX8_RATE': 'kDsp1Rx8Rate',
    'CS35L41_DSP1_TX1_RATE': 'kDsp1Tx1Rate',
    'CS35L41_DSP1_TX2_RATE': 'kDsp1Tx2Rate',
    'CS35L41_DSP1_TX3_RATE': 'kDsp1Tx3Rate',
    'CS35L41_DSP1_TX4_RATE': 'kDsp1Tx4Rate',
    'CS35L41_DSP1_TX5_RATE': 'kDsp1Tx5Rate',
    'CS35L41_DSP1_TX6_RATE': 'kDsp1Tx6Rate',
    'CS35L41_DSP1_TX7_RATE': 'kDsp1Tx7Rate',
    'CS35L41_DSP1_TX8_RATE': 'kDsp1Tx8Rate',
    'CS35L41_DSP1_CCM_CORE_CTRL': 'kDsp1CcmCoreControl',
    'CS35L41_DSP1_CORE_SOFT_RESET': 'kDsp1CoreSoftReset',
    'CS35L41_DSP1_SYS_ID': 'kDsp1SystemId',
    'CS35L41_DSP1_SYS_VERSION': 'kDsp1SystemVersion',
    'CS35L41_DSP1_SYS_CORE_ID': 'kDsp1SystemCoreId',
    'CS35L41_DSP_MBOX_1': 'kDspMailbox1',
    'CS35L41_DSP_MBOX_2': 'kDspMailbox2',
    'CS35L41_DSP_VIRT1_MBOX_1': 'kDspVirtual1Mailbox1',
    'CSPL_MBOX_CMD_RESUME': 'kMailboxCommandResume',
    'CSPL_MBOX_CMD_PAUSE': 'kMailboxCommandPause',
    'CSPL_MBOX_CMD_SPK_OUT_ENABLE': 'kMailboxCommandSpeakerOutputEnable',
    'CSPL_MBOX_STS_RUNNING': 'kMailboxStatusRunning',
    'CSPL_MBOX_STS_PAUSED': 'kMailboxStatusPaused',
    'CSPL_MBOX_STS_RDY_FOR_REINIT': 'kMailboxStatusReadyForReinitialization',
    'CS35L41_IRQ1_STATUS1': 'kIrq1Status1',
    'CS35L41_IRQ1_STATUS2': 'kIrq1Status2',
    'CS35L41_IRQ2_STATUS': 'kIrq2Status',
    'HALO_CORE_EN': 'kHaloCoreEnable',
    'HALO_CORE_RESET': 'kHaloCoreReset',
    'CS35L41_DSP1_MPU_LOCK_CONFIG': 'kDsp1MpuLockConfig',
}

def to_pascal(macro_name):
    if macro_name in SPECIAL_NAMES:
        return SPECIAL_NAMES[macro_name]
    n = macro_name
    for pfx in ('CS35L41_', 'CSPL_', 'HALO_'):
        if n.startswith(pfx):
            n = n[len(pfx):]
            break
    parts = n.split('_')
    return 'k' + ''.join(p.capitalize() if not p.isdigit() else p for p in parts)

test_cpp = '#include <cassert>\n#include <cstdint>\n#include <cstdio>\n#include "Devices/CS35L41/Hardware/Registers.hpp"\n\n'
for m, v in LEGACY_MACROS:
    test_cpp += f'#define {m} {v}\n'
test_cpp += '\nusing namespace cirrus::devices::cs35l41;\n\nint main() {\n'

test_cpp = '#include <cassert>\n#include <cstdint>\n#include <cstdio>\n#include "Devices/CS35L41/Hardware/Registers.hpp"\n\n'
for m, v in LEGACY_MACROS:
    test_cpp += f'#define {m} {v}\n'
test_cpp += '\nusing namespace cirrus::devices::cs35l41;\n\nint main() {\n'

count = 0
for macro, _ in LEGACY_MACROS:
    const_name = to_pascal(macro)
    if const_name in constexprs:
        test_cpp += f'    assert({macro} == registers::{const_name});\n'
        count += 1

# Independent policy expectations from Linux v6.12 include/sound/cs35l41.h
# and sound/pci/hda/cs35l41_hda.{c,h}. Legacy alias equality alone cannot
# detect a wrong value copied into both sides of this test.
test_cpp += '''
    static_assert(registers::kRegSoftwareReset == 0x20);
    static_assert(registers::kValSoftwareReset == 0x5A000000);
    static_assert(registers::kRegSoftwareReset != registers::kRegDeviceId);
    static_assert(registers::kPlaybackDigitalVolume == 0x8000);
    static_assert(registers::kPlaybackDspGain == 0x233);
    static_assert(registers::kPlaybackBypassGain == 0x84);
'''
test_cpp += f'    std::printf("PASS legacy register aliases (%u mapped) and independent Linux reset/volume/gain policy.\\n", {count});\n    return 0;\n}}\n'

with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    src_file = tmp / 'verify.cpp'
    exe_file = tmp / 'verify.exe'
    src_file.write_text(test_cpp, encoding='utf-8')
    cmd = ['g++', '-std=c++17', '-I' + str(ROOT / 'CirrusAudioFixup'), str(src_file), '-o', str(exe_file)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print('Compilation failed:\n', res.stderr)
        sys.exit(1)
    run_res = subprocess.run([str(exe_file)], capture_output=True, text=True)
    print(run_res.stdout.strip())
    if run_res.returncode != 0:
        print(run_res.stderr, file=sys.stderr)
        sys.exit(run_res.returncode)
