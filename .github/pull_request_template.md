<!-- Keep one purpose per PR. Use develop for normal changes; identify release PRs separately. Remove sections that do not apply and explain skipped checks. Do not mark tests you have not run as passed. -->

## Problem and result

- Problem or linked issue:
- Behavior before / after:
- Why this change fixes it:

## Scope

- [ ] Driver, playback or power management
- [ ] Board support or firmware/tuning
- [ ] I2C or native SPI
- [ ] HDA or AppleALC integration
- [ ] Collectors or diagnostics
- [ ] Build, tests or release workflow
- [ ] Documentation only

## Checks and results

Give the tested commit/artifact, commands, host OS and actual results. Link CI output where available. Write **not run** with a reason for missing checks.

| Check | Result and evidence |
| --- | --- |
| Maintained host tests: `python3 Tests/run.py` | |
| macOS Debug and Release bundles: `bash Tools/build_macos.sh` | |
| Relevant collector, parser or documentation checks | |

The macOS build script already runs the host suite. Do not repeat it just to fill this table. For documentation-only changes, check links and examples; a new hardware run is not required. For collector changes, state which OS/tool paths actually ran. A Windows host test or Mach-O object compile is not a native linked/loaded kext test.

## Hardware results

<!-- Required for driver, board, firmware, transport or audio-path changes. For other changes, remove this section and state why hardware tests do not apply. -->

- Machine, CPU and macOS version/build:
- Exact loaded kext version/configuration/commit or CI artifact; local changes:
- Bus and how identified; I2C provider build or native SPI controller/chip selects:
- Amplifier model/revision, ACPI HID/_SUB, codec/HDA SSID and channel mapping:
- AppleALC speaker-route evidence and relevant boot arguments:
- Firmware/tuning identity and measured-calibration availability:
- Collector archive, failure time and first failure, if any:

| Case | Result / not tested |
| --- | --- |
| Cold boot, attachment and expected endpoint count | |
| DSP initialization and low-volume left/right playback | |
| Playback start/stop and idle | |
| Headphone/output switching and first microphone activation | |
| Sleep/wake and playback after wake | |

For shared runtime/transport changes, report coverage of the existing Lenovo I2C and HP SPI paths separately. Name any untested path. Keep host fault-injection results separate from physical tests; do not deliberately provoke speaker protection faults on hardware to fill this table.

## Source and failure handling

<!-- For register, board, firmware, calibration or controller changes. Remove for changes that do not touch these areas. -->

- Upstream/OEM source: revision, file/lines or package identity. Explain changed addresses, masks, timings and board-specific values.
- Resource changes: filenames/hashes, physical speaker identity versus generic tuning, left/right selection and source/license notices.
- Failure/cleanup behavior: what happens on a failed read/write, timeout, partial group start, suspend or removal; supporting tests.
- Recovery: how the previous working EFI/build can be restored. List remaining boot, speaker or compatibility risks.

## Review checklist

- [ ] The diff contains only this change; no generated binaries, logs, machine dumps, unrelated formatting or private data.
- [ ] Source references and failure-handling evidence are included where hardware behavior changes.
- [ ] README, boot-argument reference and changelog match any changed public behavior.
- [ ] Test/build/hardware results above describe the actual tested artifact; skipped checks and limits are explicit.

<!-- Release PRs: also state the version, source/target branches and build/physical test evidence. Verify Info.plist, CHANGELOG and tag identity; do not publish a release from this template alone. -->
