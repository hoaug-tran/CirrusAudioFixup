# Audit status for the 1.0.1 changes

This records source changes, not hardware certification. Review and validate the
resulting bundles before publishing the version.

## Findings addressed in code

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

The service now owns the only HDA scanner. HDAController.cpp is not compiled and
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
- Split the CS35L41 firmware/monitor/playback lifecycle into a complete backend
  before adding another model. Keep the current board profile disabled on
  unverified wiring, boost circuits or speaker tuning.
- Measure worst-case workloop occupancy and event latency on hardware. Runtime
  start/stop now use a state machine and elapsed-time deadlines. Firmware
  initialization, PM quiesce and the synchronous I2C provider remain possible
  sources of latency. See [runtime transition design](runtime_transition_design.md).
- Consolidate the unused namespaced UploadPlan.hpp declarations with the active
  FirmwareUploader.hpp types before making that interface public.

Do not use READY_DSP or an HDA RUN bit as proof of audible output or measured
speaker calibration. Preserve both the original failure and later cleanup results.
