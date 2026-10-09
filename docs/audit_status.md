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
| Polling cleared faults too broadly and retried indefinitely | P2 | Faults stay latched except an isolated PLL failure with cooldown and three recovery attempts |
| Every poll rediscovered and mapped HDA, with unnecessary waits | P2 | Cache controller/BAR ownership, release on lifecycle changes, deduplicate status, remove the extra monitor sleep and yield in playback polls |

Host tests cover the affected paths using production function bodies and mocked
hardware. Power, PLL and mailbox polling still run synchronously on the workloop.
Their attempt limits do not establish a hard wall-clock deadline.

The earlier source validation on Windows passed all 11 driver/build host test scripts and compiled the
complete production source into x86_64 Mach-O objects targeting macOS 11 for
Debug and Release. git diff --check reported no whitespace errors. These objects
are not linked kext bundles; Xcode, dSYM generation and runtime checks remain
unverified for the current changes.

## Standardization

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
- Measure worst-case workloop occupancy on hardware. A non-blocking transition
  state machine and elapsed-time deadlines may be worthwhile if synchronous
  power polls delay shutdown or wake; they need timing and failure tests.
- Consolidate the unused namespaced UploadPlan.hpp declarations with the active
  FirmwareUploader.hpp types before making that interface public.

Do not use READY_DSP or an HDA RUN bit as proof of audible output or measured
speaker calibration. Preserve both the original failure and later cleanup results.
