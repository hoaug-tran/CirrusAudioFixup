# Register and value comparison

The reference is Linux **v6.12**, not an unspecified latest driver. Exact URLs and SHA-256 are retained in [the source manifest](linux_reference_sources.json). Reproduce with:

```bash
python3 Tools/audit_linux_reference.py \
  --reference-dir path/to/downloaded/linux-v6.12-files \
  --output comparison.json
```

The directory contains the files named in the manifest, flattened to their basenames. This tool does not download or trust a reference merely because of its directory name; compare hashes with the manifest. Numeric equality establishes the listed source values only. It does not establish correct board wiring, physical speaker identity, runtime ordering or acoustic behavior. The inventory contains 4,929 numeric source occurrences excluding embedded firmware arrays; occurrences are not independent semantic checks.

## Configuration and protocol decisions

| Item | Current value or rule | Reference or scope | Verdict |
| --- | --- | --- | --- |
| Silicon identity | `0x35A40`, B2 only | Linux CS35L41 identity; local B2 errata implementation | Correct in implemented scope; A0/B0 remain unsupported |
| Maximum register | `0x03804FE8`, four-byte alignment | `CS35L41_LASTREG` | Correct; larger/crossing transactions rejected before transport |
| SPI framing | Big-endian address/value, two padding bytes, read flag `0x80` | Linux SPI regmap configuration | Protocol constant; not a board default |
| I2C framing | Big-endian address/value without SPI padding or read flag | Linux I2C regmap configuration | Separate transport contract |
| Lenovo endpoints | `0x40`, `0x41` | Approved `CLSA0100`, `17AA:382B/3847` profile | Scoped; not a general Lenovo address assumption |
| Lenovo reset | `AMDI0030`, resource index 6, bits 22/23 | Approved legacy board profile | Scoped wiring; other identities cannot authorize it |
| Lenovo speaker ID | Explicit board binding `1`; unknown sentinel `UINT32_MAX` | Existing verified reference configuration | Preserves working board; automatic physical GPIO identification is not implemented |
| Physical speaker 0 | Exact identity, separate from generic resources | Linux speaker-specific filenames versus generic filenames | Fixed collision; no implicit speaker-1 fallback |
| HP boost correction | 1000 nH / 4500 mA / 24 µF | Linux `103C89C6` property correction | Correct for that SSID only |
| DSP RX5 | Internal boost `0x28`, other supported topology `0x29` | Linux HDA playback setup | Correct selector; not a voltage or temperature value |
| DSP RX6 | `0x29` | Linux HDA playback setup | Correct; explicitly restored and verified |
| Bypass RX5 / RX6 | `0x20` / `0x21` | Linux bypass signal routing | Correct; no stale DSP RX6 value retained |
| Bypass TX3 / TX4 | Zero source | Explicit local bypass policy | Establishes deterministic unused outputs |
| Playback path verification | All 13 selected path register values | Production prepare/commit gate | Rejects mismatches before unmute |
| Digital mute | `0xA678`; AMP_EN bit 0 cleared on helper stop | Linux volume encoding and amplifier enable mask | Fixed reversed enable behavior |
| Mailbox lifecycle | PAUSE `1`, RESUME `2`, SPEAKER_OUT_ENABLE `7` | Linux CSPL enum | Undefined command `8` removed; pause acknowledgement used for shutdown |
| HDA hardware stream | 48 kHz, stereo, 16/20/24-bit PCM | Enforced local HDA format gate | Deliberate supported-format contract |
| ASP timing | 3.072 MHz / 32-bit slots / 24-bit words | Implemented HDA/ASP profile | Correct within that enforced format; not arbitrary stream support |
| DSP control addresses | Resolved from firmware metadata | WMFW algorithm/control metadata | Not a universal HP/Lenovo absolute address |
| Intel LPSS clocks | 120 or 100 MHz by supported controller identity | `LPSSSPIProfiles.hpp`, Linux LPSS profiles | Controller-family constant; not ACPI bus speed |
| AMD SPI limits | V1/V2, mode 0, 64-byte frame | Implemented controller protocol | Unsupported requests rejected; physical validation remains separate |
| Measured calibration | Firmware units; nonzero uint16 R0; ambient required for boot override | Production parser and calibration contract | Never guessed from nominal impedance |
| Reset timing diagnostics | Mach absolute ticks converted to nanoseconds | Kernel time conversion API | Unit error fixed; not physical timing certification |

## Complete mapped constants

<details>
<summary>Show the full table</summary>

Source: `CirrusAudioFixup/Devices/Amplifiers/CS35L41/Hardware/Registers.hpp` and the Linux headers listed in the manifest. All numbers below were evaluated from the actual expressions rather than compared as strings.

| Local symbol | Linux symbol | Local value | Linux value | Result |
| --- | --- | --- | --- | --- |
| `kSoftwareReset` | `CS35L41_SFT_RESET` | `0x20` | `0x20` | Match |
| `kSoftwareResetValue` | `CS35L41_SOFTWARE_RESET` | `0x5a000000` | `0x5a000000` | Match |
| `kTestKeyControl` | `CS35L41_TEST_KEY_CTL` | `0x40` | `0x40` | Match |
| `kIrq1Status4` | `CS35L41_IRQ1_STATUS4` | `0x1001c` | `0x1001c` | Match |
| `kOtpBootDone` | `CS35L41_OTP_BOOT_DONE` | `0x2` | `0x2` | Match |
| `kIrq1Status3` | `CS35L41_IRQ1_STATUS3` | `0x10018` | `0x10018` | Match |
| `kOtpBootError` | `CS35L41_OTP_BOOT_ERR` | `0x80000000` | `0x80000000` | Match |
| `kIrq1RawStatus3` | `CS35L41_IRQ1_RAW_STATUS3` | `0x10098` | `0x10098` | Match |
| `kBoostEnableMask` | `CS35L41_BST_EN_MASK` | `0x30` | `0x30` | Match |
| `kProtectionMask` | `CS35L41_PROTECTION_MASK` | `0x800281c0` | `0x800281c0` | Match |
| `kIrq1Mask1` | `CS35L41_IRQ1_MASK1` | `0x10110` | `0x10110` | Match |
| `kIrq1Mask2` | `CS35L41_IRQ1_MASK2` | `0x10114` | `0x10114` | Match |
| `kIrq1Mask3` | `CS35L41_IRQ1_MASK3` | `0x10118` | `0x10118` | Match |
| `kIrq1Mask4` | `CS35L41_IRQ1_MASK4` | `0x1011c` | `0x1011c` | Match |
| `kIrq2Mask1` | `CS35L41_IRQ2_MASK1` | `0x10910` | `0x10910` | Match |
| `kIrq2Mask2` | `CS35L41_IRQ2_MASK2` | `0x10914` | `0x10914` | Match |
| `kIrq2Mask3` | `CS35L41_IRQ2_MASK3` | `0x10918` | `0x10918` | Match |
| `kIrq2Mask4` | `CS35L41_IRQ2_MASK4` | `0x1091c` | `0x1091c` | Match |
| `kPllClockControl` | `CS35L41_PLL_CLK_CTRL` | `0x2c04` | `0x2c04` | Match |
| `kDspClockControl` | `CS35L41_DSP_CLK_CTRL` | `0x2c08` | `0x2c08` | Match |
| `kGlobalClockControl` | `CS35L41_GLOBAL_CLK_CTRL` | `0x2c0c` | `0x2c0c` | Match |
| `kSerialPortRateControl` | `CS35L41_SP_RATE_CTRL` | `0x4804` | `0x4804` | Match |
| `kSerialPortFormat` | `CS35L41_SP_FORMAT` | `0x4808` | `0x4808` | Match |
| `kSerialPortFrameTxSlot` | `CS35L41_SP_FRAME_TX_SLOT` | `0x4810` | `0x4810` | Match |
| `kSerialPortFrameRxSlot` | `CS35L41_SP_FRAME_RX_SLOT` | `0x4820` | `0x4820` | Match |
| `kSerialPortTxWordLength` | `CS35L41_SP_TX_WL` | `0x4830` | `0x4830` | Match |
| `kSerialPortRxWordLength` | `CS35L41_SP_RX_WL` | `0x4840` | `0x4840` | Match |
| `kDacPcm1Source` | `CS35L41_DAC_PCM1_SRC` | `0x4c00` | `0x4c00` | Match |
| `kAspTx1Source` | `CS35L41_ASP_TX1_SRC` | `0x4c20` | `0x4c20` | Match |
| `kAspTx2Source` | `CS35L41_ASP_TX2_SRC` | `0x4c24` | `0x4c24` | Match |
| `kAspTx3Source` | `CS35L41_ASP_TX3_SRC` | `0x4c28` | `0x4c28` | Match |
| `kAspTx4Source` | `CS35L41_ASP_TX4_SRC` | `0x4c2c` | `0x4c2c` | Match |
| `kDsp1Rx1Source` | `CS35L41_DSP1_RX1_SRC` | `0x4c40` | `0x4c40` | Match |
| `kDsp1Rx2Source` | `CS35L41_DSP1_RX2_SRC` | `0x4c44` | `0x4c44` | Match |
| `kDsp1Rx3Source` | `CS35L41_DSP1_RX3_SRC` | `0x4c48` | `0x4c48` | Match |
| `kDsp1Rx4Source` | `CS35L41_DSP1_RX4_SRC` | `0x4c4c` | `0x4c4c` | Match |
| `kDsp1Rx5Source` | `CS35L41_DSP1_RX5_SRC` | `0x4c50` | `0x4c50` | Match |
| `kDsp1Rx6Source` | `CS35L41_DSP1_RX6_SRC` | `0x4c54` | `0x4c54` | Match |
| `kSerialPortHighImpedanceControl` | `CS35L41_SP_HIZ_CTRL` | `0x480c` | `0x480c` | Match |
| `kSerialPortEnables` | `CS35L41_SP_ENABLES` | `0x4800` | `0x4800` | Match |
| `kAmplifierDigitalVolumeControl` | `CS35L41_AMP_DIG_VOL_CTRL` | `0x6000` | `0x6000` | Match |
| `kAmplifierGainControl` | `CS35L41_AMP_GAIN_CTRL` | `0x6c04` | `0x6c04` | Match |
| `kGpio1Control1` | `CS35L41_GPIO1_CTRL1` | `0x11008` | `0x11008` | Match |
| `kGpio2Control1` | `CS35L41_GPIO2_CTRL1` | `0x1100c` | `0x1100c` | Match |
| `kGpioPadControl` | `CS35L41_GPIO_PAD_CONTROL` | `0x242c` | `0x242c` | Match |
| `kDsp1Rx1Rate` | `CS35L41_DSP1_RX1_RATE` | `0x2b80080` | `0x2b80080` | Match |
| `kDsp1Rx2Rate` | `CS35L41_DSP1_RX2_RATE` | `0x2b80088` | `0x2b80088` | Match |
| `kDsp1Rx3Rate` | `CS35L41_DSP1_RX3_RATE` | `0x2b80090` | `0x2b80090` | Match |
| `kDsp1Rx4Rate` | `CS35L41_DSP1_RX4_RATE` | `0x2b80098` | `0x2b80098` | Match |
| `kDsp1Rx5Rate` | `CS35L41_DSP1_RX5_RATE` | `0x2b800a0` | `0x2b800a0` | Match |
| `kDsp1Rx6Rate` | `CS35L41_DSP1_RX6_RATE` | `0x2b800a8` | `0x2b800a8` | Match |
| `kDsp1Rx7Rate` | `CS35L41_DSP1_RX7_RATE` | `0x2b800b0` | `0x2b800b0` | Match |
| `kDsp1Rx8Rate` | `CS35L41_DSP1_RX8_RATE` | `0x2b800b8` | `0x2b800b8` | Match |
| `kDsp1Tx1Rate` | `CS35L41_DSP1_TX1_RATE` | `0x2b80280` | `0x2b80280` | Match |
| `kDsp1Tx2Rate` | `CS35L41_DSP1_TX2_RATE` | `0x2b80288` | `0x2b80288` | Match |
| `kDsp1Tx3Rate` | `CS35L41_DSP1_TX3_RATE` | `0x2b80290` | `0x2b80290` | Match |
| `kDsp1Tx4Rate` | `CS35L41_DSP1_TX4_RATE` | `0x2b80298` | `0x2b80298` | Match |
| `kDsp1Tx5Rate` | `CS35L41_DSP1_TX5_RATE` | `0x2b802a0` | `0x2b802a0` | Match |
| `kDsp1Tx6Rate` | `CS35L41_DSP1_TX6_RATE` | `0x2b802a8` | `0x2b802a8` | Match |
| `kDsp1Tx7Rate` | `CS35L41_DSP1_TX7_RATE` | `0x2b802b0` | `0x2b802b0` | Match |
| `kDsp1Tx8Rate` | `CS35L41_DSP1_TX8_RATE` | `0x2b802b8` | `0x2b802b8` | Match |
| `kDsp1CcmCoreControl` | `CS35L41_DSP1_CCM_CORE_CTRL` | `0x2bc1000` | `0x2bc1000` | Match |
| `kDsp1CoreSoftReset` | `CS35L41_DSP1_CORE_SOFT_RESET` | `0x2b80010` | `0x2b80010` | Match |
| `kDsp1SystemId` | `CS35L41_DSP1_SYS_ID` | `0x25e0000` | `0x25e0000` | Match |
| `kDsp1SystemVersion` | `CS35L41_DSP1_SYS_VERSION` | `0x25e0004` | `0x25e0004` | Match |
| `kDsp1SystemCoreId` | `CS35L41_DSP1_SYS_CORE_ID` | `0x25e0008` | `0x25e0008` | Match |
| `kDspMailbox1` | `CS35L41_DSP_MBOX_1` | `0x13000` | `0x13000` | Match |
| `kDspMailbox2` | `CS35L41_DSP_MBOX_2` | `0x13004` | `0x13004` | Match |
| `kDspVirtual1Mailbox1` | `CS35L41_DSP_VIRT1_MBOX_1` | `0x13020` | `0x13020` | Match |
| `kMailboxCommandResume` | `CSPL_MBOX_CMD_RESUME` | `0x2` | `0x2` | Match |
| `kMailboxCommandPause` | `CSPL_MBOX_CMD_PAUSE` | `0x1` | `0x1` | Match |
| `kMailboxCommandSpeakerOutputEnable` | `CSPL_MBOX_CMD_SPK_OUT_ENABLE` | `0x7` | `0x7` | Match |
| `kMailboxStatusRunning` | `CSPL_MBOX_STS_RUNNING` | `0x0` | `0x0` | Match |
| `kMailboxStatusPaused` | `CSPL_MBOX_STS_PAUSED` | `0x1` | `0x1` | Match |
| `kMailboxStatusReadyForReinitialization` | `CSPL_MBOX_STS_RDY_FOR_REINIT` | `0x2` | `0x2` | Match |
| `kIrq1Status1` | `CS35L41_IRQ1_STATUS1` | `0x10010` | `0x10010` | Match |
| `kIrq1Status2` | `CS35L41_IRQ1_STATUS2` | `0x10014` | `0x10014` | Match |
| `kIrq2Status` | `CS35L41_IRQ2_STATUS` | `0x10804` | `0x10804` | Match |
| `kHaloCoreEnable` | `HALO_CORE_EN` | `0x1` | `0x1` | Match |
| `kHaloCoreReset` | `CS35L41_HALO_CORE_RESET` | `0x200` | `0x200` | Match |
| `kDsp1MpuLockConfig` | `CS35L41_DSP1_MPU_LOCK_CONFIG` | `0x2bc3140` | `0x2bc3140` | Match |
| `kDsp1MpuXmAccess0` | `CS35L41_DSP1_MPU_XM_ACCESS0` | `0x2bc3000` | `0x2bc3000` | Match |
| `kDsp1MpuYmAccess0` | `CS35L41_DSP1_MPU_YM_ACCESS0` | `0x2bc3004` | `0x2bc3004` | Match |
| `kDsp1MpuWndwAccess0` | `CS35L41_DSP1_MPU_WNDW_ACCESS0` | `0x2bc3008` | `0x2bc3008` | Match |
| `kDsp1MpuXregAccess0` | `CS35L41_DSP1_MPU_XREG_ACCESS0` | `0x2bc300c` | `0x2bc300c` | Match |
| `kDsp1MpuYregAccess0` | `CS35L41_DSP1_MPU_YREG_ACCESS0` | `0x2bc3014` | `0x2bc3014` | Match |
| `kDsp1MpuXmAccess1` | `CS35L41_DSP1_MPU_XM_ACCESS1` | `0x2bc3018` | `0x2bc3018` | Match |
| `kDsp1MpuYmAccess1` | `CS35L41_DSP1_MPU_YM_ACCESS1` | `0x2bc301c` | `0x2bc301c` | Match |
| `kDsp1MpuWndwAccess1` | `CS35L41_DSP1_MPU_WNDW_ACCESS1` | `0x2bc3020` | `0x2bc3020` | Match |
| `kDsp1MpuXregAccess1` | `CS35L41_DSP1_MPU_XREG_ACCESS1` | `0x2bc3024` | `0x2bc3024` | Match |
| `kDsp1MpuYregAccess1` | `CS35L41_DSP1_MPU_YREG_ACCESS1` | `0x2bc302c` | `0x2bc302c` | Match |
| `kDsp1MpuXmAccess2` | `CS35L41_DSP1_MPU_XM_ACCESS2` | `0x2bc3030` | `0x2bc3030` | Match |
| `kDsp1MpuYmAccess2` | `CS35L41_DSP1_MPU_YM_ACCESS2` | `0x2bc3034` | `0x2bc3034` | Match |
| `kDsp1MpuWndwAccess2` | `CS35L41_DSP1_MPU_WNDW_ACCESS2` | `0x2bc3038` | `0x2bc3038` | Match |
| `kDsp1MpuXregAccess2` | `CS35L41_DSP1_MPU_XREG_ACCESS2` | `0x2bc303c` | `0x2bc303c` | Match |
| `kDsp1MpuYregAccess2` | `CS35L41_DSP1_MPU_YREG_ACCESS2` | `0x2bc3044` | `0x2bc3044` | Match |
| `kDsp1MpuXmAccess3` | `CS35L41_DSP1_MPU_XM_ACCESS3` | `0x2bc3048` | `0x2bc3048` | Match |
| `kDsp1MpuYmAccess3` | `CS35L41_DSP1_MPU_YM_ACCESS3` | `0x2bc304c` | `0x2bc304c` | Match |
| `kDsp1MpuWndwAccess3` | `CS35L41_DSP1_MPU_WNDW_ACCESS3` | `0x2bc3050` | `0x2bc3050` | Match |
| `kDsp1MpuXregAccess3` | `CS35L41_DSP1_MPU_XREG_ACCESS3` | `0x2bc3054` | `0x2bc3054` | Match |
| `kDsp1MpuYregAccess3` | `CS35L41_DSP1_MPU_YREG_ACCESS3` | `0x2bc305c` | `0x2bc305c` | Match |
| `kLastRegister` | `CS35L41_LASTREG` | `0x3804fe8` | `0x3804fe8` | Match |

</details>

## Complete named register descriptors

<details>
<summary>Show the full table</summary>

| Register | Local address | Linux address | Result |
| --- | --- | --- | --- |
| `CS35L41_DEVID` | `0x0` | `0x0` | Match |
| `CS35L41_REVID` | `0x4` | `0x4` | Match |
| `CS35L41_FABID` | `0x8` | `0x8` | Match |
| `CS35L41_RELID` | `0xc` | `0xc` | Match |
| `CS35L41_OTPID` | `0x10` | `0x10` | Match |
| `CS35L41_SFT_RESET` | `0x20` | `0x20` | Match |
| `CS35L41_TEST_KEY_CTL` | `0x40` | `0x40` | Match |
| `CS35L41_USER_KEY_CTL` | `0x44` | `0x44` | Match |
| `CS35L41_OTP_CTRL0` | `0x500` | `0x500` | Match |
| `CS35L41_OTP_CTRL3` | `0x508` | `0x508` | Match |
| `CS35L41_OTP_CTRL4` | `0x50c` | `0x50c` | Match |
| `CS35L41_OTP_CTRL5` | `0x510` | `0x510` | Match |
| `CS35L41_OTP_CTRL6` | `0x514` | `0x514` | Match |
| `CS35L41_OTP_CTRL7` | `0x518` | `0x518` | Match |
| `CS35L41_OTP_CTRL8` | `0x51c` | `0x51c` | Match |
| `CS35L41_PWR_CTRL1` | `0x2014` | `0x2014` | Match |
| `CS35L41_PWR_CTRL2` | `0x2018` | `0x2018` | Match |
| `CS35L41_PWR_CTRL3` | `0x201c` | `0x201c` | Match |
| `CS35L41_CTRL_OVRRIDE` | `0x2020` | `0x2020` | Match |
| `CS35L41_AMP_OUT_MUTE` | `0x2024` | `0x2024` | Match |
| `CS35L41_OTP_TRIM_36` | `0x2030` | `0x2030` | Match |
| `CS35L41_PROTECT_REL_ERR_IGN` | `0x2034` | `0x2034` | Match |
| `CS35L41_OTP_TRIM_1` | `0x208c` | `0x208c` | Match |
| `CS35L41_OTP_TRIM_2` | `0x2090` | `0x2090` | Match |
| `CS35L41_GPIO_PAD_CONTROL` | `0x242c` | `0x242c` | Match |
| `CS35L41_JTAG_CONTROL` | `0x2438` | `0x2438` | Match |
| `CS35L41_PWRMGT_CTL` | `0x2900` | `0x2900` | Match |
| `CS35L41_WAKESRC_CTL` | `0x2904` | `0x2904` | Match |
| `CS35L41_PWRMGT_STS` | `0x2908` | `0x2908` | Match |
| `CS35L41_PLL_CLK_CTRL` | `0x2c04` | `0x2c04` | Match |
| `CS35L41_DSP_CLK_CTRL` | `0x2c08` | `0x2c08` | Match |
| `CS35L41_GLOBAL_CLK_CTRL` | `0x2c0c` | `0x2c0c` | Match |
| `CS35L41_DATA_FS_SEL` | `0x2c10` | `0x2c10` | Match |
| `CS35L41_TST_FS_MON0` | `0x2d10` | `0x2d10` | Match |
| `CS35L41_OTP_TRIM_4` | `0x300c` | `0x300c` | Match |
| `CS35L41_OTP_TRIM_3` | `0x3010` | `0x3010` | Match |
| `CS35L41_PLL_OVR` | `0x3018` | `0x3018` | Match |
| `CS35L41_MDSYNC_EN` | `0x3400` | `0x3400` | Match |
| `CS35L41_MDSYNC_TX_ID` | `0x3408` | `0x3408` | Match |
| `CS35L41_MDSYNC_PWR_CTRL` | `0x340c` | `0x340c` | Match |
| `CS35L41_MDSYNC_DATA_TX` | `0x3410` | `0x3410` | Match |
| `CS35L41_MDSYNC_TX_STATUS` | `0x3414` | `0x3414` | Match |
| `CS35L41_MDSYNC_DATA_RX` | `0x341c` | `0x341c` | Match |
| `CS35L41_MDSYNC_RX_STATUS` | `0x3420` | `0x3420` | Match |
| `CS35L41_MDSYNC_ERR_STATUS` | `0x3424` | `0x3424` | Match |
| `CS35L41_MDSYNC_SYNC_PTE2` | `0x3528` | `0x3528` | Match |
| `CS35L41_MDSYNC_SYNC_PTE3` | `0x352c` | `0x352c` | Match |
| `CS35L41_MDSYNC_SYNC_MSM_STATUS` | `0x353c` | `0x353c` | Match |
| `CS35L41_BSTCVRT_VCTRL1` | `0x3800` | `0x3800` | Match |
| `CS35L41_BSTCVRT_VCTRL2` | `0x3804` | `0x3804` | Match |
| `CS35L41_BSTCVRT_PEAK_CUR` | `0x3808` | `0x3808` | Match |
| `CS35L41_BSTCVRT_SFT_RAMP` | `0x380c` | `0x380c` | Match |
| `CS35L41_BSTCVRT_COEFF` | `0x3810` | `0x3810` | Match |
| `CS35L41_BSTCVRT_SLOPE_LBST` | `0x3814` | `0x3814` | Match |
| `CS35L41_BSTCVRT_SW_FREQ` | `0x3818` | `0x3818` | Match |
| `CS35L41_BSTCVRT_DCM_CTRL` | `0x381c` | `0x381c` | Match |
| `CS35L41_BSTCVRT_DCM_MODE_FORCE` | `0x3820` | `0x3820` | Match |
| `CS35L41_BSTCVRT_OVERVOLT_CTRL` | `0x3830` | `0x3830` | Match |
| `CS35L41_BST_TEST_DUTY` | `0x3900` | `0x3900` | Match |
| `CS35L41_OTP_TRIM_5` | `0x394c` | `0x394c` | Match |
| `CS35L41_OTP_TRIM_6` | `0x3950` | `0x3950` | Match |
| `CS35L41_OTP_TRIM_7` | `0x3954` | `0x3954` | Match |
| `CS35L41_OTP_TRIM_8` | `0x3958` | `0x3958` | Match |
| `CS35L41_OTP_TRIM_9` | `0x395c` | `0x395c` | Match |
| `CS35L41_VI_VOL_POL` | `0x4000` | `0x4000` | Match |
| `CS35L41_OTP_TRIM_35` | `0x400c` | `0x400c` | Match |
| `CS35L41_OTP_TRIM_34` | `0x410c` | `0x410c` | Match |
| `CS35L41_OTP_TRIM_11` | `0x4160` | `0x4160` | Match |
| `CS35L41_OTP_TRIM_10` | `0x416c` | `0x416c` | Match |
| `CS35L41_OTP_TRIM_12` | `0x4170` | `0x4170` | Match |
| `CS35L41_DTEMP_WARN_THLD` | `0x4220` | `0x4220` | Match |
| `CS35L41_DTEMP_CFG` | `0x4224` | `0x4224` | Match |
| `CS35L41_DTEMP_EN` | `0x4308` | `0x4308` | Match |
| `CS35L41_OTP_TRIM_13` | `0x4360` | `0x4360` | Match |
| `CS35L41_VPVBST_FS_SEL` | `0x4400` | `0x4400` | Match |
| `CS35L41_OTP_TRIM_14` | `0x4448` | `0x4448` | Match |
| `CS35L41_OTP_TRIM_15` | `0x444c` | `0x444c` | Match |
| `CS35L41_SP_ENABLES` | `0x4800` | `0x4800` | Match |
| `CS35L41_SP_RATE_CTRL` | `0x4804` | `0x4804` | Match |
| `CS35L41_SP_FORMAT` | `0x4808` | `0x4808` | Match |
| `CS35L41_SP_HIZ_CTRL` | `0x480c` | `0x480c` | Match |
| `CS35L41_SP_FRAME_TX_SLOT` | `0x4810` | `0x4810` | Match |
| `CS35L41_SP_FRAME_RX_SLOT` | `0x4820` | `0x4820` | Match |
| `CS35L41_SP_TX_WL` | `0x4830` | `0x4830` | Match |
| `CS35L41_SP_RX_WL` | `0x4840` | `0x4840` | Match |
| `CS35L41_DAC_PCM1_SRC` | `0x4c00` | `0x4c00` | Match |
| `CS35L41_ASP_TX1_SRC` | `0x4c20` | `0x4c20` | Match |
| `CS35L41_ASP_TX2_SRC` | `0x4c24` | `0x4c24` | Match |
| `CS35L41_ASP_TX3_SRC` | `0x4c28` | `0x4c28` | Match |
| `CS35L41_ASP_TX4_SRC` | `0x4c2c` | `0x4c2c` | Match |
| `CS35L41_DSP1_RX1_SRC` | `0x4c40` | `0x4c40` | Match |
| `CS35L41_DSP1_RX2_SRC` | `0x4c44` | `0x4c44` | Match |
| `CS35L41_DSP1_RX3_SRC` | `0x4c48` | `0x4c48` | Match |
| `CS35L41_DSP1_RX4_SRC` | `0x4c4c` | `0x4c4c` | Match |
| `CS35L41_DSP1_RX5_SRC` | `0x4c50` | `0x4c50` | Match |
| `CS35L41_DSP1_RX6_SRC` | `0x4c54` | `0x4c54` | Match |
| `CS35L41_DSP1_RX7_SRC` | `0x4c58` | `0x4c58` | Match |
| `CS35L41_DSP1_RX8_SRC` | `0x4c5c` | `0x4c5c` | Match |
| `CS35L41_NGATE1_SRC` | `0x4c60` | `0x4c60` | Match |
| `CS35L41_NGATE2_SRC` | `0x4c64` | `0x4c64` | Match |
| `CS35L41_AMP_DIG_VOL_CTRL` | `0x6000` | `0x6000` | Match |
| `CS35L41_VPBR_CFG` | `0x6404` | `0x6404` | Match |
| `CS35L41_VBBR_CFG` | `0x6408` | `0x6408` | Match |
| `CS35L41_VPBR_STATUS` | `0x640c` | `0x640c` | Match |
| `CS35L41_VBBR_STATUS` | `0x6410` | `0x6410` | Match |
| `CS35L41_OVERTEMP_CFG` | `0x6414` | `0x6414` | Match |
| `CS35L41_AMP_ERR_VOL` | `0x6418` | `0x6418` | Match |
| `CS35L41_VOL_STATUS_TO_DSP` | `0x6450` | `0x6450` | Match |
| `CS35L41_CLASSH_CFG` | `0x6800` | `0x6800` | Match |
| `CS35L41_WKFET_CFG` | `0x6804` | `0x6804` | Match |
| `CS35L41_NG_CFG` | `0x6808` | `0x6808` | Match |
| `CS35L41_AMP_GAIN_CTRL` | `0x6c04` | `0x6c04` | Match |
| `CS35L41_OTP_TRIM_16` | `0x6e30` | `0x6e30` | Match |
| `CS35L41_OTP_TRIM_17` | `0x6e34` | `0x6e34` | Match |
| `CS35L41_OTP_TRIM_18` | `0x6e38` | `0x6e38` | Match |
| `CS35L41_OTP_TRIM_19` | `0x6e3c` | `0x6e3c` | Match |
| `CS35L41_OTP_TRIM_20` | `0x6e40` | `0x6e40` | Match |
| `CS35L41_OTP_TRIM_21` | `0x6e44` | `0x6e44` | Match |
| `CS35L41_OTP_TRIM_22` | `0x6e48` | `0x6e48` | Match |
| `CS35L41_OTP_TRIM_23` | `0x6e4c` | `0x6e4c` | Match |
| `CS35L41_OTP_TRIM_24` | `0x6e50` | `0x6e50` | Match |
| `CS35L41_OTP_TRIM_25` | `0x6e54` | `0x6e54` | Match |
| `CS35L41_OTP_TRIM_26` | `0x6e58` | `0x6e58` | Match |
| `CS35L41_OTP_TRIM_27` | `0x6e5c` | `0x6e5c` | Match |
| `CS35L41_OTP_TRIM_28` | `0x6e60` | `0x6e60` | Match |
| `CS35L41_OTP_TRIM_29` | `0x6e64` | `0x6e64` | Match |
| `CS35L41_OTP_TRIM_33` | `0x7068` | `0x7068` | Match |
| `CS35L41_DIGPWM_IOCTRL` | `0x706c` | `0x706c` | Match |
| `CS35L41_DAC_MSM_CFG` | `0x7400` | `0x7400` | Match |
| `CS35L41_OTP_TRIM_30` | `0x7418` | `0x7418` | Match |
| `CS35L41_OTP_TRIM_31` | `0x741c` | `0x741c` | Match |
| `CS35L41_OTP_TRIM_32` | `0x7434` | `0x7434` | Match |
| `CS35L41_IRQ1_CFG` | `0x10000` | `0x10000` | Match |
| `CS35L41_IRQ1_STATUS` | `0x10004` | `0x10004` | Match |
| `CS35L41_IRQ1_STATUS1` | `0x10010` | `0x10010` | Match |
| `CS35L41_IRQ1_STATUS2` | `0x10014` | `0x10014` | Match |
| `CS35L41_IRQ1_STATUS3` | `0x10018` | `0x10018` | Match |
| `CS35L41_IRQ1_STATUS4` | `0x1001c` | `0x1001c` | Match |
| `CS35L41_IRQ1_RAW_STATUS1` | `0x10090` | `0x10090` | Match |
| `CS35L41_IRQ1_RAW_STATUS2` | `0x10094` | `0x10094` | Match |
| `CS35L41_IRQ1_RAW_STATUS3` | `0x10098` | `0x10098` | Match |
| `CS35L41_IRQ1_RAW_STATUS4` | `0x1009c` | `0x1009c` | Match |
| `CS35L41_IRQ1_MASK1` | `0x10110` | `0x10110` | Match |
| `CS35L41_IRQ1_MASK2` | `0x10114` | `0x10114` | Match |
| `CS35L41_IRQ1_MASK3` | `0x10118` | `0x10118` | Match |
| `CS35L41_IRQ1_MASK4` | `0x1011c` | `0x1011c` | Match |
| `CS35L41_IRQ1_FRC1` | `0x10190` | `0x10190` | Match |
| `CS35L41_IRQ1_FRC2` | `0x10194` | `0x10194` | Match |
| `CS35L41_IRQ1_FRC3` | `0x10198` | `0x10198` | Match |
| `CS35L41_IRQ1_FRC4` | `0x1019c` | `0x1019c` | Match |
| `CS35L41_IRQ1_EDGE1` | `0x10210` | `0x10210` | Match |
| `CS35L41_IRQ1_EDGE4` | `0x1021c` | `0x1021c` | Match |
| `CS35L41_IRQ1_POL1` | `0x10290` | `0x10290` | Match |
| `CS35L41_IRQ1_POL2` | `0x10294` | `0x10294` | Match |
| `CS35L41_IRQ1_POL3` | `0x10298` | `0x10298` | Match |
| `CS35L41_IRQ1_POL4` | `0x1029c` | `0x1029c` | Match |
| `CS35L41_IRQ1_DB3` | `0x10318` | `0x10318` | Match |
| `CS35L41_IRQ2_CFG` | `0x10800` | `0x10800` | Match |
| `CS35L41_IRQ2_STATUS` | `0x10804` | `0x10804` | Match |
| `CS35L41_IRQ2_STATUS1` | `0x10810` | `0x10810` | Match |
| `CS35L41_IRQ2_STATUS2` | `0x10814` | `0x10814` | Match |
| `CS35L41_IRQ2_STATUS3` | `0x10818` | `0x10818` | Match |
| `CS35L41_IRQ2_STATUS4` | `0x1081c` | `0x1081c` | Match |
| `CS35L41_IRQ2_RAW_STATUS1` | `0x10890` | `0x10890` | Match |
| `CS35L41_IRQ2_RAW_STATUS2` | `0x10894` | `0x10894` | Match |
| `CS35L41_IRQ2_RAW_STATUS3` | `0x10898` | `0x10898` | Match |
| `CS35L41_IRQ2_RAW_STATUS4` | `0x1089c` | `0x1089c` | Match |
| `CS35L41_IRQ2_MASK1` | `0x10910` | `0x10910` | Match |
| `CS35L41_IRQ2_MASK2` | `0x10914` | `0x10914` | Match |
| `CS35L41_IRQ2_MASK3` | `0x10918` | `0x10918` | Match |
| `CS35L41_IRQ2_MASK4` | `0x1091c` | `0x1091c` | Match |
| `CS35L41_IRQ2_FRC1` | `0x10990` | `0x10990` | Match |
| `CS35L41_IRQ2_FRC2` | `0x10994` | `0x10994` | Match |
| `CS35L41_IRQ2_FRC3` | `0x10998` | `0x10998` | Match |
| `CS35L41_IRQ2_FRC4` | `0x1099c` | `0x1099c` | Match |
| `CS35L41_IRQ2_EDGE1` | `0x10a10` | `0x10a10` | Match |
| `CS35L41_IRQ2_EDGE4` | `0x10a1c` | `0x10a1c` | Match |
| `CS35L41_IRQ2_POL1` | `0x10a90` | `0x10a90` | Match |
| `CS35L41_IRQ2_POL2` | `0x10a94` | `0x10a94` | Match |
| `CS35L41_IRQ2_POL3` | `0x10a98` | `0x10a98` | Match |
| `CS35L41_IRQ2_POL4` | `0x10a9c` | `0x10a9c` | Match |
| `CS35L41_IRQ2_DB3` | `0x10b18` | `0x10b18` | Match |
| `CS35L41_GPIO_STATUS1` | `0x11000` | `0x11000` | Match |
| `CS35L41_GPIO1_CTRL1` | `0x11008` | `0x11008` | Match |
| `CS35L41_GPIO2_CTRL1` | `0x1100c` | `0x1100c` | Match |
| `CS35L41_MIXER_NGATE_CFG` | `0x12000` | `0x12000` | Match |
| `CS35L41_MIXER_NGATE_CH1_CFG` | `0x12004` | `0x12004` | Match |
| `CS35L41_MIXER_NGATE_CH2_CFG` | `0x12008` | `0x12008` | Match |
| `CS35L41_CLOCK_DETECT_1` | `0x14000` | `0x14000` | Match |
| `CS35L41_DIE_STS1` | `0x17040` | `0x17040` | Match |
| `CS35L41_DIE_STS2` | `0x17044` | `0x17044` | Match |
| `CS35L41_TEMP_CAL1` | `0x17048` | `0x17048` | Match |
| `CS35L41_TEMP_CAL2` | `0x1704c` | `0x1704c` | Match |
| `CS35L41_DSP1_TIMESTAMP_COUNT` | `0x25c0800` | `0x25c0800` | Match |
| `CS35L41_DSP1_SYS_ID` | `0x25e0000` | `0x25e0000` | Match |
| `CS35L41_DSP1_SYS_VERSION` | `0x25e0004` | `0x25e0004` | Match |
| `CS35L41_DSP1_SYS_CORE_ID` | `0x25e0008` | `0x25e0008` | Match |
| `CS35L41_DSP1_SYS_AHB_ADDR` | `0x25e000c` | `0x25e000c` | Match |
| `CS35L41_DSP1_SYS_XSRAM_SIZE` | `0x25e0010` | `0x25e0010` | Match |
| `CS35L41_DSP1_SYS_YSRAM_SIZE` | `0x25e0018` | `0x25e0018` | Match |
| `CS35L41_DSP1_SYS_PSRAM_SIZE` | `0x25e0020` | `0x25e0020` | Match |
| `CS35L41_DSP1_SYS_PM_BOOT_SIZE` | `0x25e0028` | `0x25e0028` | Match |
| `CS35L41_DSP1_SYS_FEATURES` | `0x25e002c` | `0x25e002c` | Match |
| `CS35L41_DSP1_SYS_FIR_FILTERS` | `0x25e0030` | `0x25e0030` | Match |
| `CS35L41_DSP1_SYS_LMS_FILTERS` | `0x25e0034` | `0x25e0034` | Match |
| `CS35L41_DSP1_SYS_XM_BANK_SIZE` | `0x25e0038` | `0x25e0038` | Match |
| `CS35L41_DSP1_SYS_YM_BANK_SIZE` | `0x25e003c` | `0x25e003c` | Match |
| `CS35L41_DSP1_SYS_PM_BANK_SIZE` | `0x25e0040` | `0x25e0040` | Match |
| `CS35L41_DSP1_RX1_RATE` | `0x2b80080` | `0x2b80080` | Match |
| `CS35L41_DSP1_RX2_RATE` | `0x2b80088` | `0x2b80088` | Match |
| `CS35L41_DSP1_RX3_RATE` | `0x2b80090` | `0x2b80090` | Match |
| `CS35L41_DSP1_RX4_RATE` | `0x2b80098` | `0x2b80098` | Match |
| `CS35L41_DSP1_RX5_RATE` | `0x2b800a0` | `0x2b800a0` | Match |
| `CS35L41_DSP1_RX6_RATE` | `0x2b800a8` | `0x2b800a8` | Match |
| `CS35L41_DSP1_RX7_RATE` | `0x2b800b0` | `0x2b800b0` | Match |
| `CS35L41_DSP1_RX8_RATE` | `0x2b800b8` | `0x2b800b8` | Match |
| `CS35L41_DSP1_TX1_RATE` | `0x2b80280` | `0x2b80280` | Match |
| `CS35L41_DSP1_TX2_RATE` | `0x2b80288` | `0x2b80288` | Match |
| `CS35L41_DSP1_TX3_RATE` | `0x2b80290` | `0x2b80290` | Match |
| `CS35L41_DSP1_TX4_RATE` | `0x2b80298` | `0x2b80298` | Match |
| `CS35L41_DSP1_TX5_RATE` | `0x2b802a0` | `0x2b802a0` | Match |
| `CS35L41_DSP1_TX6_RATE` | `0x2b802a8` | `0x2b802a8` | Match |
| `CS35L41_DSP1_TX7_RATE` | `0x2b802b0` | `0x2b802b0` | Match |
| `CS35L41_DSP1_TX8_RATE` | `0x2b802b8` | `0x2b802b8` | Match |
| `CS35L41_DSP1_SCRATCH1` | `0x2b805c0` | `0x2b805c0` | Match |
| `CS35L41_DSP1_SCRATCH2` | `0x2b805c8` | `0x2b805c8` | Match |
| `CS35L41_DSP1_SCRATCH3` | `0x2b805d0` | `0x2b805d0` | Match |
| `CS35L41_DSP1_SCRATCH4` | `0x2b805d8` | `0x2b805d8` | Match |
| `CS35L41_DSP1_CCM_CORE_CTRL` | `0x2bc1000` | `0x2bc1000` | Match |
| `CS35L41_DSP1_CORE_SOFT_RESET` | `0x2b80010` | `0x2b80010` | Match |
| `CS35L41_DSP1_CCM_CLK_OVERRIDE` | `0x2bc1008` | `0x2bc1008` | Match |
| `CS35L41_DSP1_XM_MSTR_EN` | `0x2bc2000` | `0x2bc2000` | Match |
| `CS35L41_DSP1_XM_CORE_PRI` | `0x2bc2008` | `0x2bc2008` | Match |
| `CS35L41_DSP1_XM_AHB_PACK_PL_PRI` | `0x2bc2010` | `0x2bc2010` | Match |
| `CS35L41_DSP1_XM_AHB_UP_PL_PRI` | `0x2bc2018` | `0x2bc2018` | Match |
| `CS35L41_DSP1_XM_ACCEL_PL0_PRI` | `0x2bc2020` | `0x2bc2020` | Match |
| `CS35L41_DSP1_XM_NPL0_PRI` | `0x2bc2078` | `0x2bc2078` | Match |
| `CS35L41_DSP1_YM_MSTR_EN` | `0x2bc20c0` | `0x2bc20c0` | Match |
| `CS35L41_DSP1_YM_CORE_PRI` | `0x2bc20c8` | `0x2bc20c8` | Match |
| `CS35L41_DSP1_YM_AHB_PACK_PL_PRI` | `0x2bc20d0` | `0x2bc20d0` | Match |
| `CS35L41_DSP1_YM_AHB_UP_PL_PRI` | `0x2bc20d8` | `0x2bc20d8` | Match |
| `CS35L41_DSP1_YM_ACCEL_PL0_PRI` | `0x2bc20e0` | `0x2bc20e0` | Match |
| `CS35L41_DSP1_YM_NPL0_PRI` | `0x2bc2138` | `0x2bc2138` | Match |
| `CS35L41_DSP1_MPU_XM_ACCESS0` | `0x2bc3000` | `0x2bc3000` | Match |
| `CS35L41_DSP1_MPU_YM_ACCESS0` | `0x2bc3004` | `0x2bc3004` | Match |
| `CS35L41_DSP1_MPU_WNDW_ACCESS0` | `0x2bc3008` | `0x2bc3008` | Match |
| `CS35L41_DSP1_MPU_XREG_ACCESS0` | `0x2bc300c` | `0x2bc300c` | Match |
| `CS35L41_DSP1_MPU_YREG_ACCESS0` | `0x2bc3014` | `0x2bc3014` | Match |
| `CS35L41_DSP1_MPU_XM_ACCESS1` | `0x2bc3018` | `0x2bc3018` | Match |
| `CS35L41_DSP1_MPU_YM_ACCESS1` | `0x2bc301c` | `0x2bc301c` | Match |
| `CS35L41_DSP1_MPU_WNDW_ACCESS1` | `0x2bc3020` | `0x2bc3020` | Match |
| `CS35L41_DSP1_MPU_XREG_ACCESS1` | `0x2bc3024` | `0x2bc3024` | Match |
| `CS35L41_DSP1_MPU_YREG_ACCESS1` | `0x2bc302c` | `0x2bc302c` | Match |
| `CS35L41_DSP1_MPU_XM_ACCESS2` | `0x2bc3030` | `0x2bc3030` | Match |
| `CS35L41_DSP1_MPU_YM_ACCESS2` | `0x2bc3034` | `0x2bc3034` | Match |
| `CS35L41_DSP1_MPU_WNDW_ACCESS2` | `0x2bc3038` | `0x2bc3038` | Match |
| `CS35L41_DSP1_MPU_XREG_ACCESS2` | `0x2bc303c` | `0x2bc303c` | Match |
| `CS35L41_DSP1_MPU_YREG_ACCESS2` | `0x2bc3044` | `0x2bc3044` | Match |
| `CS35L41_DSP1_MPU_XM_ACCESS3` | `0x2bc3048` | `0x2bc3048` | Match |
| `CS35L41_DSP1_MPU_YM_ACCESS3` | `0x2bc304c` | `0x2bc304c` | Match |
| `CS35L41_DSP1_MPU_WNDW_ACCESS3` | `0x2bc3050` | `0x2bc3050` | Match |
| `CS35L41_DSP1_MPU_XREG_ACCESS3` | `0x2bc3054` | `0x2bc3054` | Match |
| `CS35L41_DSP1_MPU_YREG_ACCESS3` | `0x2bc305c` | `0x2bc305c` | Match |
| `CS35L41_DSP1_MPU_XM_VIO_ADDR` | `0x2bc3100` | `0x2bc3100` | Match |
| `CS35L41_DSP1_MPU_XM_VIO_STATUS` | `0x2bc3104` | `0x2bc3104` | Match |
| `CS35L41_DSP1_MPU_YM_VIO_ADDR` | `0x2bc3108` | `0x2bc3108` | Match |
| `CS35L41_DSP1_MPU_YM_VIO_STATUS` | `0x2bc310c` | `0x2bc310c` | Match |
| `CS35L41_DSP1_MPU_PM_VIO_ADDR` | `0x2bc3110` | `0x2bc3110` | Match |
| `CS35L41_DSP1_MPU_PM_VIO_STATUS` | `0x2bc3114` | `0x2bc3114` | Match |
| `CS35L41_DSP1_MPU_LOCK_CONFIG` | `0x2bc3140` | `0x2bc3140` | Match |
| `CS35L41_DSP1_MPU_WDT_RST_CTRL` | `0x2bc3180` | `0x2bc3180` | Match |

</details>

## OTP, errata and boost tables

Comparison includes each ordered address/value or trim destination/shift/width, not only table lengths.

| Table | Entries | Result |
| --- | --- | --- |
| OTP map 1 | 99 | Exact match |
| OTP map 2 | 99 | Exact match |
| B2 errata | 10 | Exact match |
| Boost k1 | 20 | Exact match |
| Boost k2 | 20 | Exact match |
| Boost slope | 4 | Exact match |

## Embedded payload identity

Recorded byte-for-byte comparison against the retained machine/OEM files. HP payloads match both retained Windows and Ubuntu copies. Lenovo payloads match Ubuntu copies; the Windows directories selected for this pass did not independently reproduce the earlier Lenovo Windows comparison. Matching bytes do not prove physical speaker equivalence.

| Embedded symbol | Bytes | SHA-256 | Matching retained files |
| --- | --- | --- | --- |
| `cs35l41_dsp1_spk_prot_17aa3847_wmfw` | 31060 | `e8c3f5b5b6c932652f41bd5df56a62b885edf1b00533bfeaee50c8d3e3bf8ce4` | 3 |
| `cs35l41_dsp1_spk_prot_17aa3847_spkid1_l0_bin` | 4984 | `54d87d3d0247fd9fc88b933b58497ff0efd2774464171efa4e8a91ecbf56ac0e` | 1 |
| `cs35l41_dsp1_spk_prot_17aa3847_spkid1_r0_bin` | 4984 | `77ead17486a40cad980fd7f805dc6870879c36a3b03f001cb8e961315e0c81be` | 1 |
| `cs35l41_dsp1_spk_prot_103c89c6_wmfw` | 34068 | `c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4` | 137 |
| `cs35l41_dsp1_spk_prot_103c89c6_spkid0_l0_bin` | 4000 | `7b6a432787c14a9ba1ec419672c9ff87d0101669ecda37dc14c8e902ed363c42` | 2 |
| `cs35l41_dsp1_spk_prot_103c89c6_spkid0_r0_bin` | 4000 | `2f7e769606a77ef433e5c1ca35bb4e5679b2d01326aefd9fdc11cdfcb151d81d` | 2 |

## Scope

These tables describe the listed source constants and payloads. Re-run the comparison after changing them. They do not establish support for four amplifiers, shared boost, external boost with a VSPK switch, SoundWire or another silicon revision.
