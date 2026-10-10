# CS35L41 runtime transitions

This describes the runtime source changes made on 2026-10-10. It is not a
latency measurement or a macOS hardware support declaration.

## Ownership and execution

The service owns HDA observation, route generations, IOKit power state and
the stereo barrier. `Devices/CS35L41/Playback.hpp` owns the chip-specific
runtime register phases. It receives a register-access adapter and monotonic
clock; it does not inspect CoreAudio, allocate memory or sleep.

Callbacks run on the existing service workloop. A pending start or cleanup
arms a 1 ms timer, including when event hooks are unavailable. Each callback
advances one phase per endpoint. A recognized headphone route stops periodic
monitoring after cleanup. Speaker health and unknown-route fallback retain
their separate timers.

## Muted preparation and stereo commit

The preparation phases are:

`Idle -> Resume -> Power -> Pup -> Output -> Pll -> Prepared -> Active`

Bypass skips DSP acknowledgements. Older firmware uses the existing protected
output sequence instead of the speaker-output command. Protected-register
keys are closed before returning to the workloop, including transfer failures.

Both present, initialized endpoints must reach `Prepared`. The service then
rechecks stream identity, route generation, protection and PLL state for the
pair. It writes and reads back each channel's gain and digital unmute.
A failed second commit rolls back the first endpoint as well.
The final generation check catches a route published during commit transfers.

This is a readiness barrier, not simultaneous electrical switching. I2C
transactions are sequential. A failure or event between writes can leave a
brief interval before rollback mutes both endpoints.

## Cleanup and cancellation

Runtime cleanup follows:

`Idle -> Pdn -> Output -> Pause -> Finish -> Complete`

It mutes first, removes global power, observes PDN, disables speaker output,
pauses the DSP and verifies idle registers. Bypass and older firmware skip
inapplicable commands. Cleanup continues best effort after a transfer failure,
but cannot report verified idle after that failure. Missing pause acknowledgement
invalidates DSP readiness and invokes the existing DSP-stop path.

A changed route, engine state or stream identity cancels pending preparation
and begins cleanup. Protection, I/O and DSP faults remain latched. Isolated
PLL recovery retains its bounded policy. Cleanup retries remain limited to
three attempts; idle does not silently clear a fault.

`playbackActive` remains true while cleanup is unverified, even after mute was
requested. Consumers must not interpret it as proof of audible output.

## Deadlines

| Wait | Deadline |
| --- | --- |
| Resume, output or pause acknowledgement | 5 ms |
| PUP or PDN acknowledgement | 100 ms |
| PLL readiness | 20 ms |
| Prepared endpoint waiting for its peer | 100 ms |

Deadlines are armed after the relevant register sequence. Status reads also
check elapsed time after the synchronous transfer. A late mailbox response
cannot establish timely acknowledgement. A delayed timer does not receive
a fresh retry window merely because few callbacks ran.

The provider's addressed transfer is still synchronous. A deadline cannot
interrupt that call; it is checked when control returns. These numbers do not
guarantee end-to-end audio switching within a specific duration.

## Plan completion boundaries

| Work item | Source status | Remaining evidence or work |
| --- | --- | --- |
| Runtime transition state machine | Implemented for playback start and cleanup | PM quiesce, initialization and transport remain synchronous |
| Coordinated stereo preparation | Implemented readiness barrier and pair rollback | Physical stereo timing and switching latency |
| IRQ, faults and sleep/hibernate | Existing masked status checks and synchronous sleep/wake retained | No verified GPIO IRQ provider contract; runtime hibernate/regcache parity is not implemented |
| Windows analysis | Mailbox helper and direct callers inspected | Full caller semantics, tuning selection, volume ramp, calibration and IRQ/hibernate reconstruction remain incomplete |
| Backend separation | Runtime register phases extracted and unsupported models rejected | Firmware discovery/upload/boot and several helpers remain service-owned |
| Target verification | Host suites and macOS-target object compilation | Xcode link/dSYM, actual hook loading, provider identity and hardware playback |

The current host is Windows and has no `xcodebuild`. No remote macOS executor
is configured for this pass. Existing CI builds both Xcode configurations and
checks bundles and symbols, but no CI result for these uncommitted changes
is claimed. No commit, push or release was performed.

## Regression coverage

Runtime tests execute production callback bodies and the actual backend.
They cover each cancellation phase, independent left/right register maps,
a delayed right endpoint, a failed second commit, elapsed PUP/PDN deadlines,
late I2C mailbox responses, unsupported-model rejection and idle without I2C.
Single-transfer fault sweeps cover start and asynchronous runtime cleanup.
The existing firmware, PM, calibration and protection suites remain required.

The final host run passed all 14 check suites and the firmware reproducer.
Start sweeps covered 117 and 123 transfer positions; cleanup sweeps covered
34 and 36 positions for the two firmware branches. Debug and Release main
objects compiled as x86_64 Mach-O relocatables targeting macOS 11.

Host mocks cannot verify bus latency, speaker excursion, acoustic response,
interrupt delivery or whether the IOAudioFamily hooks load on target macOS.
