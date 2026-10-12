# Audit status for the 1.0.1 changes

This records source changes, not hardware certification. Review and validate the
resulting bundles before publishing the version.

## Findings addressed in code

### Target-machine evidence collected on 2026-10-10

Local Xcode Debug and Release bundles now build and pass the host regressions,
metadata, architecture, dSYM UUID and OpenCore vtable checks. Direct Xcode
builds also reject the original namespaced IOKit vtable regression. The earlier
object-only validation statements below describe historical passes, not the
current local build status.

After the vtable and acknowledgement-poll fixes, the supplied 19:21 kernel log
and 19:22 IORegistry snapshot show both amplifiers in `READY_DSP`, protection
`OK`, active playback and `IOAUDIO_FAMILY_EVENTS`. Both IOAudioFamily hooks
report installed. No `CirrusAudioFixup: ERROR:` message appears among the 2,652
Cirrus messages in the supplied kernel log. The user confirms audible internal
speakers and working headphone insertion/removal with rapid speaker recovery.
This establishes those flows on this machine; it does not certify other boards
or OS versions. `log-other.txt` contains no Cirrus messages and is not independent
driver-health evidence.

Four completed preparation cycles span approximately 75.3, 77.1, 87.2 and
93.5 ms from left-channel `PLAYBACK_PREPARE` to pair `PLAYBACK_ACTIVE`. These
are driver preparation intervals in a verbose Debug capture, not jack-to-sound
latency or worst-case bounds. No sleep/wake test or acoustic measurement is
established by these captures. The remaining matrix and parity work below stays
open. P1/P2 labels denote finding priority, not completed hardware milestones.

| Finding | Priority | Current change |
| --- | --- | --- |
| Read-only startup could toggle GPIO reset | P1 | Read-only and probe-only modes return before reset-controller discovery or writes |
| Four endpoints shared stereo slots and tuning | P1 | Profiles permit two endpoints; channel identity selects slots and tuning |
| Branch pushes could overwrite a versioned release | P1 | Publishing requires a matching new tag; versions are checked and existing releases are not overwritten |
| Legacy debug phases did not stop the production flow | P2 | Named stage gates are in the initialization path; the monitor stays disabled and invalid names reject startup |
| Polling cleared faults too broadly and retried indefinitely | P2 | I/O, DSP and protection faults remain latched. Isolated PLL recovery is permitted separately. Recognized speaker transitions have a bounded preparation window; legacy clock-wait fallback retains its backoff policy |
| Every poll rediscovered and mapped HDA, with unnecessary waits | P2 | Cache controller/BAR ownership, release on lifecycle changes, deduplicate status, remove the extra monitor sleep and yield in playback polls |
| Headphone playback triggered repeated amplifier power retries | P2 | IOAudioFamily output and engine hooks queue workloop events. A recognized headphone route stops the periodic timer. Speaker transitions cancel obsolete work before unmute. Unrecognized selectors retain an explicit timer fallback |

Host tests cover the affected paths using production function bodies and mocked
hardware. Runtime playback preparation and cleanup now yield between phases.
PUP/PDN, PLL and mailbox waits use monotonic deadlines rather than sleep loops.
Synchronous I2C calls still occupy the workloop. Initialization, IOKit power
quiesce and final teardown retain synchronous sequencing.

The earlier source validation on Windows passed all 11 driver/build host test scripts and compiled the
complete production source into x86_64 Mach-O objects targeting macOS 11 for
Debug and Release. git diff --check reported no whitespace errors. These objects
are not linked kext bundles; Xcode, dSYM generation and runtime checks remain
unverified for the current changes.

## P2 latency and P3 follow-up

Preparation avoids redundant active-health and log-only I2C reads while keeping
phase protection and pair-commit verification. Quiet health skips timestamp and
ASP logging reads. Per-endpoint prepare durations are published in IORegistry;
a faster end-to-end transition is not claimed until the new bundle is booted.

`WMFWFormat.hpp` and `WMFWUploadPlan.hpp` are now the canonical production types.
Parser and uploader consume them instead of defining incompatible duplicates.
This closes the duplicate-type cleanup; further service/driver extraction is
still open. IRQ provider forwarding has been reviewed against the supplied custom
VoodooI2C source. An opt-in shared GPIO84 protection path is implemented with fault latching,
polling fallback and PM disable/reconfigure. Target IRQ delivery and runtime
hibernate are still open, as is validation on other OS/board combinations. P3 is not complete.

### Current deployment and validation matrix

| Target | Evidence | Pending |
| --- | --- | --- |
| Reference Lenovo, macOS 15.5 (24F74), prior fixed Debug | User-confirmed speakers/headphone switching; supplied clean driver log | Repeated sleep/wake and cold boot |
| Same target, optimized Release | All host suites, Xcode bundle/dSYM/metadata/vtable checks; installed EFI SHA-256 `6104aead881f1395d00e372fc3bb0b5780a295ed001252077e9faa6e97a06865` | Next boot, audible output and repeated timing samples |
| Reference Linux capture | Two amps on shared GPIO84; register/IRQ configuration captured | IRQ count was zero; no observed protection delivery |
| Other boards/OS versions | No new target evidence | Full validation |

Deployment removes only `-cirrusdbg` and `-cirrusdumptrace` from the EFI config.
macOS denied a direct NVRAM write; the existing OpenCore NVRAM/Delete entry for
`boot-args` will permit its quiet Add value on next boot. Existing non-Cirrus
config arguments are preserved. IRQ is not enabled for this deployment.
The prior kext, config and boot arguments are saved under
`build/LoadDebug/before-p2-release`. The running kernel still uses the old bundle
until reboot; deployment is not new-bundle hardware verification.

## Standardization

The [CS35L41 parity audit](linux_parity_audit.md) records the 2026-10-10 upstream
comparison. This pass corrects the software-reset address/command and preserves
the PCM high-pass bit at digital unity. Quiet Release skips log-only snapshots
and unsolicited full dumps. Required protection and playback readbacks remain.
Pending stereo preparation keeps its short bounded timer even if one channel
is already active. These changes do not establish acoustic or Windows parity.

Optional `.bincfg` companions now import into the executable alongside WMFW/BIN.
Selection parses each channel's gain before upload and playback verifies the
encoded gain register. The existing reference entry has no companion and keeps
the Linux default. Importer SHA-256 comments record supplied byte identity;
they do not verify provenance. Malformed/duplicate profiles reject import.
Preparation and runtime cleanup now advance across timer callbacks. Route
cancellation rolls back without manufacturing a hardware timeout. The separate
initialization/PM mailbox helper remains synchronous. No runtime filesystem
loader or helper was added.

The service now owns the only HDA scanner. The empty alternate-scanner source was removed and
contains no alternate implementation. The shared header contains observation
state; the format decoder contains no MMIO scanner.

Initialization dispatches by model before invoking initializeCS35L41(). Other
models are rejected. This establishes an initialization boundary; firmware,
monitoring and playback are still CS35L41-specific. A second model needs its own
complete lifecycle and tests before the registry can admit it.

Debug defaults to routine logging; Release requires -cirrusdbg. Errors, fault
records and safety checks remain in both. Xcode generates matching dSYMs, and CI
is configured to check UUIDs and publish a separate symbols archive.

The project and target configurations now use macOS 11.0 with a pinned
MacKernelSDK. README distinguishes that build target from tested OS support.
Tahoe's missing AppleHDA dependency remains an audio-stack requirement, not
something the amplifier kext can solve through its PCI scanner.

Additional fixes found during this pass: the firmware debug stop frees its parsed
image, and read-only startup publishes its driver verdict without waiting for a
power transition. Comments describe ownership, thread context, hardware ordering
and failure handling above the relevant code.

The documentation/feedback pass passed all 12 host scripts, including a new
tools regression for CLI exit codes and collector capture failures. It corrects
right-channel firmware validation and explicit missing-file handling, and checks
collector Bash syntax. It documents Tahoe restoration prerequisites, the untested
VoodooHDA combination, and controlled microphone-pop evidence collection. Collector
runtime and command output still require validation on each affected macOS build.

## Work still requiring evidence or a separate change

The 2026-10-10 Windows reference pass read the eight maintained Markdown
documents and compared OEM resources, installed configuration and a focused
`csaudio.sys` mailbox disassembly. All 14 `check_*.py` suites and the firmware
reproducer passed. Main-driver Debug and Release Mach-O object compilation
also passed. The Windows virtual status bank differs from Linux's physical
CSPL status bank; the kext retains the Linux interface and tests that a reply
in the other bank cannot establish command success. Mailbox read failures
retain their diagnostic stage and I/O result. README dependency, test-list
and audit links were corrected without changing its visual structure.

The subsequent runtime pass extracted CS35L41 start/cleanup phases, introduced
monotonic deadlines and coordinated stereo commit. All 14 host suites and the
firmware reproducer passed again. Start fault sweeps cover 117/123 operations
for the two firmware branches; runtime cleanup sweeps cover 34/36 operations.
Debug and Release x86_64 Mach-O object compilation passed. These remain
relocatable objects, not linked or loaded bundles.

This pass does not complete Windows caller reconstruction, the OEM effects
algorithm, IRQ delivery or asynchronous initialization/PM sequencing. See the
[Windows reference audit](windows_driver_audit.md) for the exact binary hash,
instruction addresses and limits of the disassembly findings.

- Build and link both bundles in Xcode, then verify their symbols and load them on
  the target macOS machine. Host compilation cannot replace those steps.
- Record an Intel/AMD and OS validation matrix with cold boot, stereo playback,
  start/stop, headphones and repeated sleep/wake. No such complete matrix is
  established by this checkout.
- Establish the host audio stack on Tahoe before claiming Tahoe speaker support.
- Investigate the reported first microphone-activation pop using timestamped
  pre/post snapshots. Its origin is not established; do not raise gain or alter
  codec power sequencing solely from that report.
- The CS35L41 firmware/monitor/playback lifecycle now belongs to its independent
  driver. Keep new board profiles disabled on unverified wiring, boost circuits
  or speaker tuning.
- Measure worst-case workloop occupancy and event latency on hardware. Runtime
  start/stop now use a state machine and elapsed-time deadlines. Firmware
  initialization, PM quiesce and the synchronous I2C provider remain possible
  sources of latency. See [runtime transition design](runtime_transition_design.md).
- WMFWUploadPlan.hpp now supplies namespaced production types through compatibility
  aliases consumed by WMFWUploader.hpp; no parallel unused schema remains.

Do not use READY_DSP or an HDA RUN bit as proof of audible output or measured
speaker calibration. Preserve both the original failure and later cleanup results.

## Boot identity failure found on 2026-10-11

The supplied failing boot log shows Release attached at 4.388 s, both CS35L41
endpoints detected, then firmware discovery failed at 4.487/4.580 s with SSID
zero. The live registry has `IOHDACodecFunctionSubsystemID = 0x17AA3847` under
`IOHDACodecFunction`. This is not an amplifier detect failure. Discovery omitted
that class/property and permanently latched missing firmware when identity was
not yet published. Debug logging delays could change boot timing but are not a
correct synchronization mechanism.

Discovery now reads number/data FunctionSubsystemID and skips known non-Realtek
codec identities and zero entries. An absent SSID defers firmware upload and
keeps output disabled, retrying from the timer every 100 ms for at most 30 s.
When identity arrives, complete chip initialization resumes. A known unsupported
SSID, protection/transport failure or identity timeout remains a fault. No
reference SSID is automatically substituted. Host regressions cover actual
registry lookup, deferred stereo bringup, late identity, no repeated init and
bounded timeout. The new installed bundle still requires a target reboot test.

## Compatibility hardening on 2026-10-11

The live reference machine remains READY_DSP with both endpoints safe-idle and
protection OK before these changes. The new source gates board reset and full
initialization on CLSA0100 plus HDA PCI subsystem 17AA382B/17AA3847; the live
machine exposes 17AA382B. Codec firmware identity remains a separate deferred
lookup. An absent controller identity disables initialization rather than guessing.
Legacy discovery performs reads only on unsupported boards and no longer invents
present endpoints. A partial pair reports INCOMPLETE_TOPOLOGY and performs no
initialization. Unsupported-board PM and IRQ paths do not configure the amps.
Output selectors published after hook attachment are re-seeded while route is
unknown, with live observations retaining precedence.

Existing maintained suites now execute production discovery for missing/unknown
endpoints, production bringup/PM for unsupported and incomplete boards, and
production event-source refresh for late selectors. Hardware cold boot and
sleep/wake of the resulting binary still require validation. Debug and Release builds passed the maintained host suites, metadata, x86_64,
dSYM UUID and OpenCore vtable checks. The Release binary SHA-256 is
`eac87c472f9731acde14584156df7610c862389a2dfb187ae4fd7ec5c46a528f`.
The verified bundle was copied to EFI after backing up the working bundle under
`build/LoadDebug/hardening/CirrusAudioFixup.before-hardening.kext`. The installed
binary matches the built hash; reboot/load and hardware validation are pending.


## Framework ownership completion — 2026-10-12

Independent production CS35L41 ownership is complete: owned live state, lifecycle,
firmware/calibration, silicon helpers and playback/monitoring are in its driver.
The IOService uses generic host, register and lifecycle contracts. Legacy and
manifest discovery consume registry-owned metadata. No software ownership TODO
remains from the earlier bridge milestone. See the final section of
[extensible driver](extensible_driver.md) for implementation and verification.

Read-only inspection established matching loaded/archive custom VoodooI2C UUIDs
and the addressed-transfer field offsets and dispatch behavior. Compile-time ABI
assertions pin that layout. See [provider evidence](provider_abi_evidence.md).
Source fork/revision, firmware redistribution licensing, native controller and
new-AMP contracts, target speaker identity and physical validation remain open;
these are evidence requirements, not completed support claims. No provider code
or firmware bytes were imported and no bundle was deployed for this refactor.


Native SPI controller ownership/PM has also moved to module factories and the
common `NativeSPIControllerOwner` contract. Synthetic independent communication
owners cover shared targets, release ordering, resume rollback and selected
factory rejection without fallback. Existing Intel/AMD engines and their
physical evidence gates are unchanged.


## Attachment/debug hardening — 2026-10-12

The host Intel/AMD × I2C/SPI matrix now covers successful runtime/playback,
unsupported controller/private ABI, absent AMP registration, wrong silicon and
unsupported direct ACPI I2C. Bus selection follows validated resource type rather
than ACPI class or CPU vendor. SPI controller-only inventory uses no invented AMP
register format. I2C preflight blocks unsupported ABI/readable wrong silicon before
board reset. Unknown HID/phase handling is null-safe and bounded.

Attachment diagnostics separate communication, AMP, policy, binding and PM,
preserve first/latest native returns, type indices and rate-limit repeats.
CPU/provider/model metadata, debug flags, identity probe traces and raw SPI IDs
are published. Failed/unbound diagnostic attachments no longer map HDA or rearm
background polling. The source does not expand Info.plist matching, board
allowlists, firmware resources or physical support claims. See
[attachment matrix](attachment_matrix.md) for the complete contract and observed
AMD/custom-I2C baseline on the older loaded bundle.

### Direct ACPI I2C factory policy follow-up

A selected ACPI I2C adapter now uses the same pre-reset identity preflight and
unregistered-AMP authorization gate as provider-backed I2C. Previously that
branch returned immediately after controller creation. Regression coverage
executes the actual service branch for successful identity reads, an unsupported
addressed function and an absent AMP registration. The unsupported AMP path
performs no identity transfer and retains controller-only diagnostics. This adds
no native I2C implementation or physical support claim.

The follow-up passed all 29 audit suites, normal/sanitized attachment matrix,
formatter and Debug/Release bundle checks (metadata, vtables, x86_64 and matching
binary/dSYM UUIDs). No resulting bundle was installed or loaded.

## Diagnostics and quiet Release audit — 2026-10-12

The audit found root diagnostic context could be overwritten by cleanup, and
quiet Release still recorded every transfer and could read failure snapshots.
Root stage/register/expected/actual/native return now remain immutable. Failure
counts saturate. Trace recording is allocated only for verbose or explicitly
requested tracing; quiet failures retain essential reporting without snapshot
reads. Diagnostic allocation failure no longer aborts safe service startup.

Trace snapshots copy bounded history while locked, then allocate/format/publish
outside the recorder lock; text concatenation is linear. Failure history freezes
before extra snapshot reads. Trace labels use endpoint indices without channel
inference. Workloop/timer/command-gate setup errors and PM registration failures
report the precise failed operation and available native return. See
[diagnostic readiness audit](diagnostics.md#diagnostic-readiness-audit-2026-10-12)
for the boundary evidence and remaining physical observability limits.

Verification passed: all 29 audit suites, additional production trace allocation
fault/wrap/lock tests in normal and ASan/UBSan runs, formatter, Debug/Release
builds, metadata, vtables, x86_64 and binary/dSYM UUID matching. Xcode membership
now ignores Finder's `.DS_Store` metadata while retaining checks for actual files.
Only the existing SDK 15.5 kmod startup-object/deployment-floor linker warnings
remain. The final Release binary SHA-256 is
`128e1b575044ea333dc7623695804ba239efc3f1cbed415f824119a6edbcb908`,
UUID `DF07200B-4379-31C9-B11F-93F31BB12E76`; Debug UUID is
`C3E831AE-05E4-38EE-B4E6-EE0C06831EB9`. Neither bundle was installed or loaded.
Physical latency/performance and electrical root-cause resolution are not proven
by these host checks.

## Kernel driver naming audit — 2026-10-12

All production filenames/folders were reviewed by responsibility. Amplifier
implementations now live in `Devices/Amplifiers/<Chip>` with corresponding C++
namespaces. Factory, registration, driver registry/context, host adapter, amplifier
manager and SPI controller owner names replace the earlier implementation-role
terminology. `SiliconState` distinguishes chip state from endpoint views.

A comparison against the previous commit verified all 74 production C++ files
retain rename-normalized non-whitespace parity, allowing include sorting. No
register sequence, firmware bytes, provider contract or hardware authorization
changed. See [kernel driver naming](kernel_driver_naming.md) for the selected
conventions, upstream references and module ownership rules.

Final naming verification passed all 28 maintained suites plus the optional
Windows audit (29 total), normal/sanitizer module and fault harnesses, pinned
formatter for all 74 production files, and Debug/Release bundle gates: plist,
metadata, vtable pairs, x86_64 and matching binary/dSYM UUIDs. Generic I2C/SPI
service operations now use Communication names. The naming gate rejects the
obsolete role term in source names/symbols. The three user-supplied HP collection
files remain untouched and unstaged.

Final Debug UUID: `6670947A-557E-3938-8046-AFD36E3CC12A`.
Final Release UUID: `3D144221-8F67-3AC0-B533-356EF4AF9294`;
SHA-256: `6bad5a529b80d6c16b1a9be0add9e04d6d59b300e8ee12572622738d5dd392a4`.
Only the existing SDK kmod startup-object deployment-floor warnings remain.
No bundle was deployed or loaded for this naming change.

## Verified EFI Debug deployment — 2026-10-12

The final source/build audit passed again (29 suites including the optional
Windows audit, formatter and both bundle gates). The verified Debug bundle from
source commit `7a8c46e` was installed into the mounted reference OpenCore EFI;
all copied files match the build. The previous bundle/config and new dSYM are
backed up under `build/EFIDeployment/20261012-235422-7a8c46e-Debug`. Config/NVRAM
and other kexts are unchanged. Debug verbosity is enabled by the build itself.

Installed UUID `6670947A-557E-3938-8046-AFD36E3CC12A`, executable SHA-256
`74bcf8e5879ae2231a4bf6e65068e4c62a184407c01b460bbc1d03b1061fc754`.
The kernel still runs old UUID `2564C407-4ED3-379A-8693-8C529B52434D` until reboot;
new-build hardware validation is pending. See [EFI boot validation](efi_boot_validation.md)
for exact identities, preparation evidence, boot checks and rollback.

## Delayed board identity boot regression — 2026-10-13

The supplied `/Volumes/EFI/log.txt` shows the intended Debug UUID booted, with
service attachment at 1.592 s. The HDA PCI SSID was still zero and initialization
was blocked at 1.608 s by BOARD_NOT_AUTHORIZED. The runtime then parked without
retrying board discovery. IOAudioFamily hooks appeared at 21.239 s; live HDEF
registry now exposes subsystem vendor 17AA and subsystem ID 382B, already in the
existing allowlist. This establishes a missing dependency retry, not AMP damage
or an unsupported physical board. `log-other.txt` contains no Cirrus messages;
the unrelated graphics allocation errors do not establish the amplifier cause.

Missing board identity now enters WAITING_BOARD_IDENTITY for at most 30 seconds,
with 100 ms dependency polling on the workloop. It performs no reset or amplifier
initialization while identity is absent. Once identity arrives, it revalidates the
unchanged board policy and communication/identity preflight before reset, discovery
and bringup. Unknown boards, expired identity and failed preflight remain blocked.
Repeated unchanged profile resolution does not repeat property/log publication.
Faulted/unbound diagnostic attachments still park after the dependency resolves
or expires. Pending verdicts stay distinguishable from unsupported identity.

The author registry property was never removed. The earlier diagnostic cleanup
removed the console banner. A concise `Author: Tran Kinh Hoang (hoaugtr)` line is
restored for Debug or `-cirrusdbg`; quiet Release remains free of routine banners.
Regression executes the production retry for identity at 21 s, no early reset,
once-only bringup, unsupported identity, deadline, factory failure, preflight
identity rejection and both transport discovery branches under ASan/UBSan.

Full maintained suite and optional Windows audit passed, with final targeted
attachment/bringup checks and normal/ASan/UBSan retry coverage. Debug/Release,
formatter, metadata, vtables, x86_64 and matching dSYM UUID gates passed. Corrected
Debug source commit `e0b46ee` is installed in EFI with exact file-hash verification,
UUID `B0FF6C51-3DDC-3E0D-899B-03743BD14534`, executable SHA-256
`cd53c2f093a9e39f856706309bf8c9fb431f72d0448438cd500766005db7d990`.
Backup/config/failing logs are under
`build/EFIDeployment/20261013-013712-e0b46ee-Debug`. Config/NVRAM are unchanged;
no live reload or reboot occurred. Corrected-build output remains unverified until
another boot. The original known-working backup is retained separately.
