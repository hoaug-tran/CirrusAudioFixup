# Windows CS35L41 reference audit

Inspection date: 2026-10-10. This report compares OEM files, installed registry
configuration and embedded resources. It does not certify runtime register
values, acoustic output or complete Windows driver equivalence.

## Device and installed configuration

The connected Cirrus device is `ACPI\CLSA0100\1`, with service `csaudio`.
Its class configuration references `oem5.inf`, section `CONF_0000.NT`,
version `22.7.28.977`. The retained base INF is `csaudio.inf`.
The Realtek device identifies codec `10EC:0287` and subsystem `17AA:3847`.
Both Cirrus extensions, `oem4.inf` and `oem36.inf`, appear on the device.

The published base files `C:\Windows\INF\oem5.inf` and `oem8.inf` were absent.
Their original package directories remained readable under DriverStore.
Exporting by those published names failed. The reason for the missing files
was not established. No Windows driver or registry setting was changed.

The Cirrus class registry contained:

| Setting | Stored value | Interpretation boundary |
| --- | --- | --- |
| DefaultTuningSet | 4 | Base and extension tables identify the protection tuning set |
| VolumeRampRate | 4 | Driver-specific code; not a verified duration |
| DefaultSampleRate | 3 | Driver-specific code; not interpreted as a frequency |
| ForceAmpSafeState | 1 | Stored policy, not proof of a power sequence |
| EnableHibernation | 1 | Configured; transition behavior remains unverified |
| EnableIntHandler | 1 | Configured; not proof of interrupt delivery |
| EnableEsdResetPolling | 0 | This optional polling feature is disabled |
| EnableAutoCal / EnableAutoInit | 0 / 0 | Do not infer factory calibration is missing |
| DefaultChannelMap | `00 01 01 00` | Do not copy directly into macOS ASP slots |

Firmware `B2\1` names `halo_cspl_RAM_revB2_29.41.0.wmfw`.
Playback branches `B2\4\1\0` and `B2\4\1\1` name the Veco left/right
`LS639_0905` files. Luxshare branches are also configured. These mappings
do not prove which physical speaker vendor the Windows driver selected.
All four playback branches store gain `1146880` (`0x00118000`).
The corresponding INF describes this value as +17.5 dB.
This Windows configuration encoding is not a CS35L41 register value.

## Exact embedded payload matches

The executable's `17aa3847` firmware and `spkid1` coefficients match these
Windows files byte for byte. No payload replacement is needed for this profile.

| Embedded payload | OEM file | Bytes |
| --- | --- | --- |
| `17aa3847_wmfw` | `halo_cspl_RAM_revB2_29.41.0.wmfw` | 31060 |
| `17aa3847_spkid1_l0_bin` | `Lenovo_Y760_Veco_Left_Spk_Tuning_LS639_0905.bin` | 4984 |
| `17aa3847_spkid1_r0_bin` | `Lenovo_Y760_Veco_Right_Spk_Tuning_LS639_0905.bin` | 4984 |

SHA-256 values, in the same order:

```text
e8c3f5b5b6c932652f41bd5df56a62b885edf1b00533bfeaee50c8d3e3bf8ce4
54d87d3d0247fd9fc88b933b58497ff0efd2774464171efa4e8a91ecbf56ac0e
77ead17486a40cad980fd7f805dc6870879c36a3b03f001cb8e961315e0c81be
```

Exact coefficient matches occur in the `oem36.inf` export. The older
`oem4.inf` package contains `LS639_0329` tuning names. Both packages being
present is not a reason to replace the embedded profile.
No loose `.bincfg` file was found in the exported packages.
Windows provides gain configuration through INF and registry here.

## Static driver evidence

`csaudio.sys` is an x86-64 Windows kernel binary, 322984 bytes.
Its SHA-256 is:

```text
b96bda112b4fabc6a790a8d80b225c15733d3c070d80cf5aff5ab705206834ae
```

Its imports include `IoRegisterPlugPlayNotification`, event waits,
`PsCreateSystemThread`, `KeDelayExecutionThread`, registry and file access,
firmware-environment variable access, and the WDF loader.
These mechanisms do not reconstruct the playback call flow.
A notification import does not prove headphone events directly switch speakers.

Retained strings include `CirrusSmartAmpCalibrationData`, `LoadDeltaTuning`,
`VolumeRampRate`, `AMP_GAIN`, `BASE_DRE.AMP_GAIN` and `EnableHibernation`.
The CodeView record names a private `csaudio.pdb`; that PDB was not supplied.
Strings do not establish call order, register masks or wait durations.

## Consequences for this kext

### Mailbox command path

For the binary hash above, disassembly at VA `0x14000977c` accepts an
endpoint index, command and expected status. It writes the command to
`0x13020` at `0x140009811`, reads that command bank back, and distinguishes
negative command responses. The subsequent loop reads `0x13024` at
`0x1400098ae`, compares the expected status, and handles `0xFFFFFFFF` and
`0x00FFFFFF` error values. An elapsed shared-time comparison bounds the loop.
Its deadline depends on a global value at `0x14003818c`; the observed constant
alone is not a verified complete timeout. The loop contains no explicit sleep,
but the register-read helper can itself wait. This does not prove busy-loop
CPU consumption or end-to-end headphone-switch latency.

[Linux v6.12's CSPL helper](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41-lib.c)
writes the same command bank but reads physical status `0x13004`.
The kext retains this Linux contract. Switching it to the Windows status
address without verifying the firmware/interface behavior would be unsafe.
Regression tests now reject a virtual-bank-only acknowledgement and both
physical-bank error sentinels. The kext checks the response immediately,
then sleeps only when a response remains pending, retaining five retry waits.
Mailbox read failures explicitly retain the mailbox stage and I/O result.

Repeat the focused inspection with LLVM installed; this command is read-only:

```powershell
& 'C:\Program Files\LLVM\bin\llvm-objdump.exe' -d --no-show-raw-insn --start-address=0x14000977c --stop-address=0x140009980 'C:\Windows\System32\DriverStore\FileRepository\csaudio.inf_amd64_3abbd251e5a04b6f\csaudio.sys'
```

These addresses and the DriverStore directory apply only to the inspected
binary. Resolve a different package and compare its hash before reusing them.
Caller reconstruction, interrupt dispatch and hibernate sequencing remain
incomplete; this function alone does not specify the complete Windows driver.

### Direct mailbox callers

The same binary contains these direct calls. Values below are taken from
the argument registers immediately before each call, not from retained strings.

| Call instruction VA | Command argument | Expected status argument |
| --- | --- | --- |
| `0x1400040c6` | 6 | 1 |
| `0x140007dc5` | 7 | 1 |
| `0x140007e42` | 2 | 0 |
| `0x14000851b` | 1 | 1 |
| `0x1400090bb` | 4 | 2 |
| `0x1400090db` | 3 | 0 |
| `0x1400090ea` | 1 | 1 |
| `0x14000a860` | 2 | 0 |

The branch preceding `0x140007dc5` compares a stored version field with
`0x1d3100`. Its alternative writes `0xF9` to `0x742c` and `0x00580941` to
`0x7438`. The surrounding code loops over endpoints, checks raw status
`0x10090` with mask `0x01000000`, applies the command/alternative sequence,
then loops over endpoints for command 2. The pair-wide phases are observable;
their connection to an actual headphone notification remains unproven.

Command 7 expects 1 in this Windows path, unlike the Linux physical-bank
speaker-enable check. The stored version encoding also differs from the
kext's parsed firmware version. Neither status values nor version thresholds
are copied into the Linux-based interface merely to resemble this binary.

The path containing `0x14000851b` issues command 1 to each endpoint, calls
another per-endpoint helper, then performs an indirect wait. That callee and
the wait's import mapping were not fully reconstructed here. This is not
evidence of a verified end-to-end suspend or volume-ramp duration.

The exported `NH3ProductSettings.cab` contains
`Devices/17AA3847_InternalSpeakers.nsx`. Its XML identifies
`subsys_17AA3847` and `InternalSpeakers`. The profile enables the device
optimization filter, master limiter, attenuator and stereo reduction.
It contains separate encoded left/right optimization filters, attenuation
thresholds and frequency-response data. Stream and master input/output gain
fields are all 0 dB. These are profile settings, not proof of active APO state.
The encoded filter layout and processing algorithm have not been reconstructed.
Writing these bytes into amplifier registers or DSP memory would be incorrect
without a documented mapping to that DSP firmware's controls.

The reference profile already embeds OEM-identical Veco firmware/coefficient
bytes. Its default PCM gain of 17.5 dB agrees with the OEM configuration.
This does not establish equal Windows/macOS middle-slider loudness.
Host codec attenuation, effects processing and calibration remain separate.
Realtek and A-Volute extensions match `17AA3847`; this does not prove an APO
is enabled or reproduce its algorithm on macOS.

Do not raise gain, rewrite coefficients or import a calibration-mode BIN to
compensate for perceived loudness. Calibration-mode files contain algorithm
parameters, not proof of measurements for this physical pair of speakers.
Runtime start/cleanup now use asynchronous acknowledgement phases and a
coordinated stereo readiness barrier; see [runtime transition design](runtime_transition_design.md).
Remaining improvements include initialization/PM sequencing, power-state
restoration and verified IRQ/fault handling.
These require macOS provider contracts and CS35L41 hardware semantics.
Uninterpreted Windows INF bytes are not a safe implementation specification.

## Repeat the comparison

Run from the repository root with Python 3. No third-party packages are required.
The utility reads inputs and prints JSON. It does not modify the driver,
registry, firmware or embedded arrays.

```powershell
python Tools/audit_windows_payloads.py --packages 'D:\CirrusDriverExport'
python Tests/check_windows_payloads.py
```

Optional `--driver` paths add SHA-256 and filtered ASCII/UTF-16 strings.
Missing inputs fail explicitly. Exact matches establish byte identity only.
