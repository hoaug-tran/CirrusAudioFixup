# Volume, EQ and calibration evidence

This audit separates matching resources from matching sound. It covers the
embedded `17AA3847 / spkid1` profile and the exported Lenovo Windows packages.
It does not certify another speaker vendor or measured acoustic performance.

## What each layer controls

| Layer | Linux | Windows evidence | CirrusAudioFixup |
| --- | --- | --- | --- |
| User volume | Host codec/mixer and userspace policy | Realtek driver and Windows endpoint/effects chain; transfer curve not recovered | Host audio stack owns volume; no replacement curve |
| Amplifier gain | Default DSP PCM code 17; profile gain can override | OEM INF identifies `0x00118000` as 17.5 dB | Default DSP gain 17.5 dB; embedded companion override supported |
| DSP tuning | Board/speaker-specific WMFW and coefficients | Halo 29.41 firmware and Veco LS639_0905 coefficients | Byte-identical firmware and separate L/R coefficients |
| Calibration | Per-device EFI data written into four firmware controls | Calibration labels/configuration are present; actual measured values not established here | EFI or explicit measured boot arguments, with write/readback verification |
| Host effects | Distribution/application dependent; HDA driver does not establish Nahimic parity | OEM Nahimic device profile contains optimization, attenuation and limiter settings | Nahimic processing is not implemented |

Windows INF gain encoding is not a register value. Do not write `0x00118000`
directly to AMP_GAIN. Equal slider percentages do not establish equal PCM level,
amplifier gain, loudness or frequency response.

## Resource identity

The exported `oem36.inf` package matches these embedded resources:

| Resource | Bytes | SHA-256 |
| --- | ---: | --- |
| Halo firmware | 31060 | `e8c3f5b5b6c932652f41bd5df56a62b885edf1b00533bfeaee50c8d3e3bf8ce4` |
| Veco left tuning | 4984 | `54d87d3d0247fd9fc88b933b58497ff0efd2774464171efa4e8a91ecbf56ac0e` |
| Veco right tuning | 4984 | `77ead17486a40cad980fd7f805dc6870879c36a3b03f001cb8e961315e0c81be` |

The package also contains Luxshare tuning. Package presence does not prove which
speaker vendor Windows selected on the physical machine. Keep resource selection
bound to the board/speaker identity rather than choosing the newest file.

## Nahimic profile inspection

`17AA3847_InternalSpeakers.nsx` has SHA-256
`9d0b735439d1293c2cbe76ce368962207123fdad332aebb5171ddf72baab9bd7`.
Its static settings enable device optimization, master limiting and attenuation.
Stream/master input and output gain settings are all 0 dB.

The decoded optimization payload is 8192 bytes per channel. Both channels have
the same SHA-256: `f633fe7fe81823fbfd939bd4200e4bb9b0796197772f1e36490d260bd1dd20fc`.
Interpreting each payload as little-endian float32 produces 2048 finite values.
That interpretation is a hypothesis, not a documented filter ABI.

The attenuation threshold payloads contain 72 bytes each and differ by channel.
Under the same float interpretation, left values span -30 to -10; right values
span approximately -30.48 to -11.08. Frequency-response payloads contain 28 bytes
each and also differ. Their frequency bins and units have not been established.

Do not equate these tables with a volume-slider curve. Do not infer the sample
rate, FIR layout, processing order, headroom or active APO state from base64.
Copying these bytes into Cirrus DSP memory has no established control mapping.
Running a 2048-tap PCM filter would also require an audio-processing path; this
I2C lifecycle driver does not currently own PCM buffers.

## Calibration correction

The Linux helper writes ambient, R0, status, then checksum `R0 + 1`.
The kext follows that order and verifies every written value before DSP boot.
The checksum is firmware protocol data, not an acoustic certificate or file CRC.

Linux skips entries with both timestamp words zero. The kext now does the same:
an empty EFI slot is reported as `NOT_AVAILABLE` and causes no calibration writes.
Malformed buffers still fail validation. Explicit measured boot arguments remain
available when EFI calibration is absent.

Targeted EFI entries now use silicon UID lookup before channel-index fallback.
The UID is assembled from DIE_STS2 (high word) and DIE_STS1 (low word), matching Linux.
An unmatched nonzero target is not applied. A zero target or zero silicon UID
permits index fallback, as in Linux. Entries with empty timestamps remain unused.
UID matching also works when the channel index exceeds the EFI entry count.

Wildcard-only tables do not require UID register reads. Targeted tables require
both reads to succeed; a transport failure reports `UID_READ_FAILED` and blocks
calibration writes. This is deliberately stricter than treating a failed read as
an unknown UID and applying an index entry. Reads occur during firmware setup,
not in a new background monitor. Host tests cover reordered entries, mismatched
targets, wildcards, empty timestamps, zero UID and both UID-read failures.
Calibration values from host-test fixtures must never become a shipping preset.

## Windows volume interpretation

Microsoft documents the endpoint scalar as a nonlinear audio-tapered value.
Its curve can change across Windows versions. A scalar of 0.5 does not mean
half signal amplitude or a fixed attenuation in dB.

`GetMasterVolumeLevel` reports current attenuation in dB; `GetVolumeRange` reports
the endpoint's minimum, maximum and increment. Those values provide a better
comparison basis than UI percentages, but one endpoint snapshot cannot recover
the complete curve or the effects chain. No Windows-to-macOS curve has been
established from the available static evidence. This is a comparison limitation,
not a confirmed cause of the reported mid-slider loudness difference.

## Reproduce the static comparison

Run from the repository root, using an existing Python 3 installation and actual
exported package/profile paths. This command reads inputs without modifying them:

```powershell
python Tools/audit_windows_payloads.py --packages 'D:\CirrusDriverExport\oem36.inf' --nahimic 'build/verification/windows-nahimic/Devices/17AA3847_InternalSpeakers.nsx'
python Tests/check_windows_payloads.py
python Tests/check_calibration.py
```

The profile path is an extracted audit input, not a kext installation requirement.
No new runtime disk access or additional polling is introduced.

## What is still needed for acoustic parity

A recovered volume transfer function needs actual endpoint/codec attenuation
mapping, not a single registry gain or slider position. A faithful EQ port needs
the effects format, sample rate, stage order, limiter behaviour and a suitable
PCM-processing implementation. Calibration needs the speaker's own measured data
and correct identity binding.

Acoustic certification additionally requires controlled L/R measurements at
matched digital input levels: frequency response, SPL, distortion and protection
behaviour across volume and temperature. Static code, hashes and host tests cannot
replace those measurements. No gain increase or unmeasured EQ preset is applied
by this audit.

## Primary sources

- [Linux CS35L41 HDA driver, v6.12](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.c): firmware controls, configuration and calibration integration.
- [Linux Cirrus amplifier helper, v6.12](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs-amp-lib.c): calibration write order, checksum, timestamp and UID selection.
- [Microsoft endpoint scalar](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nf-endpointvolume-iaudioendpointvolume-getmastervolumelevelscalar): nonlinear volume mapping and version-dependent curve.
- [Microsoft endpoint volume range](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nf-endpointvolume-iaudioendpointvolume-getvolumerange): attenuation range and step size in decibels.
- Local OEM evidence: exported `csaudioext.inf`, Halo firmware, Veco coefficients and extracted `17AA3847_InternalSpeakers.nsx`. These establish static content, not active Windows processing.
- [Windows driver audit](windows_driver_audit.md) and [Linux parity audit](linux_parity_audit.md): package provenance and lifecycle differences.
