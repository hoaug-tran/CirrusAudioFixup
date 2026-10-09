# CirrusAudioFixup

[![Build](https://github.com/hoaug-tran/CirrusAudioFixup/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/hoaug-tran/CirrusAudioFixup/actions/workflows/build.yml)

CirrusAudioFixup is an x86_64 macOS kernel extension for the I2C-connected CS35L41 speaker amplifiers used by the reference Lenovo Legion configuration. It handles amplifier setup, firmware upload and playback power transitions. AppleALC and the host audio driver still provide the audio device and PCM route.

Incorrect boost settings or speaker tuning can damage the hardware. Keep a known-good EFI, begin at low volume, and read the [bring-up guide](docs/safe_bringup_protocol.md) before testing a new configuration. DSP bypass removes acoustic protection; it is not a safe default for an unknown board.

## Hardware and macOS scope

The implemented backend is CS35L41. The current personality matches a `VoodooI2CDeviceNub` whose `IOName` is `CLSA0100`.

| Configuration | What the source currently provides |
| --- | --- |
| Lenovo Legion 7 16ACHg6, SSID `17AA:3847` | Reference stereo profile, AMD `AMDI0030` reset quirk, embedded firmware and left/right tuning |
| Lenovo Legion 5 Pro 16ACH6H, SSID `17AA:382B` | Explicit translation to the `17AA:3847` resource; validate speaker and boost equivalence on the actual machine |
| `CLSA0101` / `CSC3551` | Staged profiles with automatic initialization disabled; no shipped matching personality |
| Other Intel or AMD boards | Porting required: provider, reset wiring, audio route, boost circuit and tuning must be verified |
| CS35L45 / CS35L51 / CS35L56 / other amplifier models | No implemented backend; unknown models are rejected before initialization |
| Realtek ALC codecs | Managed by AppleALC and the host audio stack, not by this kext |

Current profiles permit two endpoints: left `0x40` and right `0x41`. Four state slots are reserved in the service, but four-channel playback is not supported. The channel field selects ASP slots and speaker tuning.

The HDA gate accepts 48 kHz stereo PCM with 16-, 20- or 24-bit samples. The amplifier uses fixed ASP slot timing. A 44.1 kHz *hardware stream* is rejected; CoreAudio may resample a 44.1 kHz source before it reaches HDA.

### macOS compatibility

Both Xcode configurations target macOS 11.0 and build only for x86_64. This is a build target, not a claim that every OS and board combination has passed playback testing.

| macOS | Compatibility status |
| --- | --- |
| Big Sur 11 | Deployment target; target-machine load, playback and power testing still required |
| Monterey 12 | Compatibility target; target-machine validation still required |
| Ventura 13 | Original deployment floor; the current changes still need target-machine validation |
| Sonoma 14 | Compatibility target; target-machine validation still required |
| Sequoia 15 | Compatibility target; target-machine validation still required |
| Tahoe 26 | Conditional target; the AppleHDA dependency needs a working, validated solution |

[MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) supports targeting older XNU kernels with current Xcode. The source uses IOKit/KPI dependencies at the Big Sur generation. Building against those headers does not prove that a kext loads, that its transport works, or that speakers play on a particular OS.

### Tahoe: restore and validate the host audio stack first

[AppleALC upstream](https://github.com/acidanthera/AppleALC#features) notes that macOS 26 removed AppleHDA in DP2. For the AppleHDA/AppleALC route used by this project, restore a compatible AppleHDA using a patch maintained for your exact OS build, then verify that AppleHDA and AppleALC load and the codec exposes the intended speaker route. Having AppleHDA on disk is not sufficient.

[Dortania's Tahoe guide](https://github.com/dortania/OpenCore-Install-Guide/blob/master/extras/tahoe.md) describes AppleHDA restoration and the security/root-volume caveats. OCLP and community patches are external audio-stack solutions, not dependencies installed or validated by CirrusAudioFixup. Upstream OCLP targets supported Mac models; do not assume its patch policy applies unchanged to a Hackintosh. Follow the patch project's requirements and recheck the audio stack after an OS update. This README does not prescribe SIP changes or a particular patch bundle.

CirrusAudioFixup's PCI fallback can locate an HDA controller, but cannot replace AppleHDA, create a CoreAudio device or program the codec route. A restored host stack makes Tahoe a feasible test target; it does not establish tested amplifier support on every Intel or AMD system.

VoodooHDA with CirrusAudioFixup has not been tested. A running HDA descriptor alone does not establish compatible codec routing, BCLK, ASP timing or power transitions. No compatibility or incompatibility claim is made for that combination.

Tahoe still lists [supported Intel Macs](https://support.apple.com/en-us/122867). AMD systems separately depend on suitable OpenCore CPU patches; [AMD Vanilla](https://github.com/AMD-OSX/AMD_Vanilla#supported-macos-versions) lists Big Sur through Tahoe. Neither fact establishes amplifier compatibility on an arbitrary Intel or AMD laptop. Apple silicon and native AMD macOS support are outside this project's scope.

## Installation

For a new build, use the artifacts for the exact commit you intend to test, or a versioned [release](https://github.com/hoaug-tran/CirrusAudioFixup/releases). Do not mix a Debug kext and symbols from different commits.

The checked-in [docs/Kexts.zip](docs/Kexts.zip) is a historical bundle. Its plists contain CirrusAudioFixup 1.0.0, AppleALC 1.9.9 and VoodooI2C 2.9.1. It does not contain the current 1.0.1 changes, and its version numbers do not establish compatibility with another macOS release.

Prepare the existing OpenCore audio stack:

1. Install a compatible Lilu and AppleALC build for your OS. VirtualSMC belongs to the wider Hackintosh setup.
2. Install the [custom VoodooI2C fork](https://github.com/hoaug-tran/VoodooI2C) that implements the addressed `VoodooI2CTransferToAddress` platform function. Keep the controller and plugin configuration required by that fork and your machine.
3. Add CirrusAudioFixup after VoodooI2C in `Kernel -> Add`. Each entry needs the correct bundle, executable and plist paths.
4. Make sure the amplifier nub appears in the IOService plane with `IOName=CLSA0100`. Do not assume every laptop places it under the same ACPI path.
5. Configure the reference ALC287 route with `alcid=16`, or the equivalent four-byte `layout-id` value `10 00 00 00`. Other codecs and boards need their own route.

CirrusAudioFixup has no direct Lilu link dependency. Lilu is needed by AppleALC. The custom I2C provider is accessed through a platform function, not a linked `VoodooI2CTransferToAddress` symbol.

Begin with `-cirrusro -cirrusdbg` when checking a new installation. Confirm the provider, silicon identity and resource selection before allowing initialization or playback. See the [bring-up guide](docs/safe_bringup_protocol.md) for stage-by-stage checks.

## What the driver does

During startup, the service selects a board profile, validates the requested debug phase and performs the permitted board reset. Read-only and probe-only modes skip that reset. Silicon identity is checked before initialization.

Initialization dispatches by model. The CS35L41 path resets the chip, waits for OTP boot, applies trim and errata, configures clocks and ASP, selects channel tuning, uploads firmware and checks the DSP. It finishes in verified muted idle. No other model is routed through that sequence.

A timer observes HDA output stream descriptors at a nominal 50 ms interval. It checks controller state, stream tag and format; multiple running output streams are treated as ambiguous. The retained PCI service and BAR mapping are reused until suspend, removal, invalid topology or teardown.

Playback requires more than HDA RUN. The monitor verifies PLL, DSP/mailbox, power acknowledgements and output registers before marking a channel active. Stream loss, read failures and protection faults trigger cleanup. Most faults remain latched. An isolated PLL failure can retry after a cooldown, at most three times before a successful start resets the retry count.

Sleep and stop share the workloop gate with the timer. Suspend attempts mute, power-down and DSP halt, then releases the HDA cache. Wake repeats software-reset initialization and firmware staging. It does not repeat the board GPIO reset performed by `start()`.

Register and heartbeat checks do not measure the PCM signal, speaker excursion or audible output. A successful verdict must still be checked against the actual route and speakers.

## Boot arguments

Flags with a value of `0` are treated as disabled. Debug builds enable routine logging by default; `-cirrusdbg=0` does not override that build default.

| Argument | Actual behavior |
| --- | --- |
| `-cirrusoff` | Reject attachment before hardware initialization |
| `-cirrusdbg` | Enable routine `IOLog` messages in Release; already enabled by default in Debug |
| `-cirrusro` | Skip GPIO reset, initialization and register writes; collect a one-shot register snapshot. Reads still send an I2C register-address prefix |
| `-cirrusnodsp` | Skip firmware and coefficient upload; use the explicit DAC bypass path without DSP acoustic protection |
| `-cirrusphase=<name>` | Stop after a selected initialization stage; playback stays disabled |
| `-cirruslegacyprobe` | Permit fixed-address discovery outside automatic profile policy; diagnostic use only, not evidence of board support |
| `-cirrusprobe` | Record the flag in `CirrusBootArgParsed`; currently does not enable a separate transaction-log mode |
| `-cirrusdelay=<ms>` | Set the read-only snapshot delay, default 100 ms. Does not replace the normal 15 ms startup wait |
| `-cirrusdiag` | Enable additional initialization register snapshots and CRC checks; essential runtime checks remain active without it |
| `-cirruscompact` | Shorten register snapshot text by omitting register names; the readable-register set is unchanged |
| `-cirrusdumptrace` | Publish `Cirrus_Trace_Dump` during the read-only snapshot. Failure trace properties are captured separately |
| `-cirrusnocal` | Skip measured calibration injection. Firmware and channel tuning still upload in DSP mode |
| `-cirrusssid=<value>` | Override the detected subsystem ID for resource selection; does not verify speaker compatibility |
| `-cirrusspkid=<value>` | Override speaker ID selection; the embedded reference resource uses speaker ID 1 |
| `-cirruscalr0=<value>` | Fallback measured R0 value for either channel when no EFI calibration is available |
| `-cirruscalr0l=<value>` | Left-channel measured R0 override; takes precedence over the shared value |
| `-cirruscalr0r=<value>` | Right-channel measured R0 override; takes precedence over the shared value |
| `-cirruscalstatus=<value>` | Calibration status for boot-argument calibration, default 1 |
| `-cirruscalambient=<value>` | Required measured ambient value when supplying R0 through boot arguments |

Valid phases are `probe`, `otp`, `errata`, `clock`, `asp`, `gpio`, `platform`, `firmware` and `dsp`. Unknown names, including legacy numeric phase names, reject startup before reset. The `firmware` phase stops before DSP boot; the `dsp` phase halts the core after boot checks.

Calibration values must come from the hardware's calibration procedure and use the firmware's units. Do not copy an impedance value from another laptop. Missing measured calibration is reported as `NOT_AVAILABLE`; `READY_DSP` does not certify measured calibration or validate the acoustic tuning.

## Diagnostics

Inspect the driver service, not just its ACPI device:

```bash
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
sudo /usr/bin/log show --last 1h --style syslog --info --debug --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
sudo /usr/bin/log stream --level debug --style syslog --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
```

The first log command reads the retained last hour; adjust the interval if needed. The second watches new messages until Ctrl-C. Neither recreates messages that were not retained. Empty `ioreg` output means no matching service was found, not a successful health check. Release routine messages require `-cirrusdbg` at boot. Use the collector and its kernel-buffer fallback when the unified log is empty. See [diagnostic collection and audio feedback](docs/diagnostics.md) for expected results and microphone-pop checks.

The `Cirrus_Build_Configuration` property identifies Debug or Release. CI bundle plists also carry the build commit and configuration. Check them when comparing logs or reporting a panic.

| Driver verdict | Meaning |
| --- | --- |
| `READY_DSP` | Present amplifiers passed initialization and DSP checks; audible output and measured calibration remain separate |
| `READY_BYPASS_EXPLICIT` | Initialization completed with explicit DSP bypass; acoustic protection is absent |
| `DEBUG_PHASE_HALTED` | Initialization stopped at the requested debug boundary; playback is disabled |
| `FAULT_LATCHED` | An initialization, I/O, DSP, protection or playback fault is retained |
| `FAILED_INIT` | Initialization readiness was not established |
| `OUTPUT_DISABLED_DSP_UNVERIFIED` | Initialization state does not establish a usable DSP or explicit bypass |
| `READ_ONLY` | Diagnostic attachment without initialization or register writes |
| `SUSPENDED` | Service is in its low-power state |

Per-channel `Cirrus_Playback_Verdict_left/right` values describe playback:

- `SAFE_IDLE_VERIFIED`: the driver's idle register checks passed.
- `ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED`: active register checks passed; downstream routing and sound remain unconfirmed.
- `CLEANUP_UNVERIFIED`: cleanup did not establish the expected idle state.
- `FAULT_LATCHED_OUTPUT_DISABLED`: a fault remains and output cleanup was verified.

Read `Cirrus_Diag_FirstFailure_*`, `Cirrus_Diag_LatestFailure_*`, `Cirrus_HDA_Status` and the trace properties together. The first failure records the original problem; a later cleanup failure must not replace it. The full failure list lives in [DiagnosticTypes.hpp](CirrusAudioFixup/Diagnostics/DiagnosticTypes.hpp).

Collect evidence before rebooting:

```bash
sudo bash Tools/collect_macos.sh
```

Run this from the repository root on the affected macOS installation. The script prints its output directory and records command exit codes and empty output in `status.txt`; it does not create a ZIP archive. Some optional tools or properties can be absent. Review collected NVRAM and hardware identifiers before sharing them. A missing controller, unsupported format or ambiguous stream should be investigated in the audio stack before forcing another firmware SSID.

## Debug and Release builds

The two builds intentionally use the same hardware sequencing, checks, fault recorder and recovery limits. Do not remove safety checks or allow additional boost settings in Release.

| Setting | Debug | Release |
| --- | --- | --- |
| Compiler optimization | `-O0` | `-Os` |
| Routine logging at attachment | Enabled | Disabled; enable with `-cirrusdbg` |
| Error logging | Enabled | Enabled |
| Deep register diagnostics | Explicit `-cirrusdiag` | Explicit `-cirrusdiag` |
| Debug information | Matching dSYM | Matching dSYM for optimized code |
| Installed-product stripping | Disabled | Disabled |
| Hardware safety checks | Unchanged | Unchanged |

CI verifies binary/dSYM UUIDs and packages symbols separately in `SYMBOLS.zip`. Keep the symbols for the exact build; an optimized Release panic still needs matching symbols. There are no Debug-only panic assertions in the current driver.

Branch pushes produce test artifacts. Public releases require a new matching `vX.Y.Z` tag, plist versions and changelog entry. Versioned assets include checksums and build metadata. Existing releases are not overwritten.

## Building and testing

Use Xcode on macOS and the configured [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK). From the repository root:

Clone the same SDK revision used by CI once, before building:

```bash
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
git -C MacKernelSDK checkout --detach 7af1933c27aefcbdf4809ee44478829aad30f9c1
```

If an SDK checkout already exists, inspect its state before changing its revision. The project uses its kernel headers and x86_64 libkmod rather than the SDK bundled with Xcode.

```bash
xcodebuild -project CirrusAudioFixup.xcodeproj \
  -target CirrusAudioFixup -configuration Release -sdk macosx \
  CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO \
  CONFIGURATION_BUILD_DIR="$PWD/build/Release" build
```

Repeat with `Debug` and `build/Debug` for the diagnostic build. These are unsigned OpenCore-injection artifacts, not a standalone macOS installer.

Host tests require Python 3, Bash and a C++17 compiler available as `g++` (Bash is included with macOS; on Windows the tools test uses Git for Windows Bash):

```bash
python3 Tests/reproduce_host.py
python3 Tests/check_registers.py
python3 Tests/check_transport.py
python3 Tests/check_diagnostics.py
python3 Tests/check_hda.py
python3 Tests/check_calibration.py
python3 Tests/check_bringup.py
python3 Tests/check_runtime.py
python3 Tests/check_architecture.py
python3 Tests/check_release.py
python3 Tests/check_build_policy.py
python3 Tests/check_tools.py
```

Injected failures deliberately print errors. Check each process's exit code and final PASS result. These tests exercise production code with mocked buses and IOKit objects; they cannot establish a real I2C transaction, loaded kext or speaker output.

Before declaring a board/OS combination supported, record the commit, macOS build, CPU/platform, audio codec/layout, VoodooI2C build and firmware/tuning identity. Test cold boot, quiet idle, low-volume stereo playback, repeated start/stop, headphone switching and sleep/wake. Capture both channel verdicts and first/latest failures. See [CONTRIBUTING.md](CONTRIBUTING.md).

The completed source fixes and remaining validation work are listed in [audit_status.md](docs/audit_status.md).

## Porting another board or amplifier

Start with read-only evidence from macOS and a working Linux configuration. Record ACPI HID, I2C endpoints, silicon/revision, HDA subsystem ID, reset wiring, boost topology and speaker/tuning IDs.

For CS35L41, add a board profile and matching personality only after checking that evidence. An SSID override alone is not a port. Validate the reference audio route and each channel separately.

The import utility uses `--bin-l` and `--bin-r`. Validate first:

```bash
python3 Tools/import_firmware.py --codec cs35l41 --ssid 0x103C89B5 \
  --wmfw path/to/cs35l41-dsp1-spk-prot-103c89b5.wmfw \
  --bin-l path/to/left.bin --bin-r path/to/right.bin \
  --spkid 1 --validate-only
```

Replace the example SSID and every `path/to/...` argument with your actual resource identity and files. This checks basic container headers and alignment, not complete block parsing, DSP identity, board compatibility or runtime validation. Invalid right-channel input must fail just like invalid left-channel input. Omitting `--bin-r` deliberately reuses left-channel tuning; do that only when the source profile specifies identical tuning. Review the import utility before using its write mode and keep a recoverable source commit.

For another amplifier model, implement its identity, register access, reset/trim, clocks, power, DSP and playback lifecycle before adding it to the supported registry. Add its own dispatch case; never send a new chip through `initializeCS35L41()`. The monitor and firmware selection are still CS35L41-specific and also need separate backend handling before another model can be enabled.

## Sources and license

Register definitions and sequencing draw on the Linux CS35L41 HDA/ASoC drivers. This is a macOS port with board-specific behavior, not a claim of bit-for-bit parity with every upstream path. Firmware and tuning resources are embedded in [Firmware.hpp](CirrusAudioFixup/Devices/CS35L41/Resources/Firmware.hpp); preserve their provenance and applicable licenses when replacing them.

See [LICENSE](LICENSE) for the non-commercial distribution terms. Paid system-setup services are permitted under the license when no fee is charged for this software itself; selling the software or paid EFI bundles is not.

Maintained by [Tran Kinh Hoang](https://github.com/hoaug-tran). Thanks to the VoodooI2C team, Acidanthera, Cirrus Logic and the Linux audio maintainers for their code and documentation. The [reference EFI](https://github.com/hoaug-tran/Lenovo-Legion-7-16ACHG6-Hackintosh) and its [historical audio issue](https://github.com/hoaug-tran/Lenovo-Legion-7-16ACHG6-Hackintosh/issues/2) provide board context, not a validation matrix for every macOS release.
