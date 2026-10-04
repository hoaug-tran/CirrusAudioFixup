from pathlib import Path
import re
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
REG_HPP = ROOT / 'CirrusAudioFixup/Devices/CS35L41/Hardware/Registers.hpp'
content = REG_HPP.read_text(encoding='utf-8')

# Extract constexpr and macros
constexprs = dict(re.findall(r'constexpr\s+(?:uint8_t|uint32_t)\s+([A-Za-z0-9_]+)\s*=\s*([^;]+);', content))
macros = dict(re.findall(r'#define\s+([A-Za-z0-9_]+)\s+([^\n/]+)', content))

if not constexprs:
    print("ERROR: No constexpr registers found in Registers.hpp", file=sys.stderr)
    sys.exit(1)

if not macros:
    print("ERROR: No register macros found in Registers.hpp", file=sys.stderr)
    sys.exit(1)

# Compile a verification binary with g++ that tests value equality
test_cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "Devices/CS35L41/Hardware/Registers.hpp"

using namespace cirrus::devices::cs35l41;

int main() {
'''

# Map macro to constexpr name
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

count = 0
for macro in macros:
    const_name = to_pascal(macro)
    if const_name in constexprs:
        test_cpp += f'    assert({macro} == registers::{const_name});\n'
        count += 1

test_cpp += f'''
    std::printf("All register constants verified equal ({count} mapped).\\n");
    return 0;
}}
'''

with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    src_file = tmp / 'verify_registers.cpp'
    exe_file = tmp / 'verify_registers.exe'
    src_file.write_text(test_cpp, encoding='utf-8')

    cmd = ['g++', '-std=c++17', '-I' + str(ROOT / 'CirrusAudioFixup'), str(src_file), '-o', str(exe_file)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("Compilation failed:\n", res.stderr, file=sys.stderr)
        sys.exit(1)

    run_res = subprocess.run([str(exe_file)], capture_output=True, text=True)
    if run_res.returncode != 0:
        print("Assertion failed:\n", run_res.stderr, file=sys.stderr)
        sys.exit(1)

    print(run_res.stdout.strip())
    print("OK: Register verification passed completely.")
