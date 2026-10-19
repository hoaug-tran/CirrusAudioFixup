# CS35L41 runtime transitions

Playback starts muted, waits for both channels and cancels if the host route changes. This page describes ordering and deadlines; the numbers are not measured audio-switching latency.

## Ownership and execution

The service owns HDA observation, route generations, IOKit power state and
the stereo barrier. `Devices/Amplifiers/CS35L41/Playback.hpp` owns the chip-specific
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

Deadlines are armed after the relevant register sequence. Each poll observes status before declaring an expired wait. A ready response
is accepted even when its synchronous transfer completed after the deadline,
as in Linux polling; an unsatisfied response expires without a new retry window. A delayed timer does not receive
a fresh retry window merely because few callbacks ran.

The provider's addressed transfer is still synchronous. A deadline cannot
interrupt that call; it is checked when control returns. These numbers do not
guarantee end-to-end audio switching within a specific duration.

## Preparation timing

Pending preparation skips the recurring active-health/logging sweep. Its phase
checks, protection observation and stereo commit checks remain required.
Quiet operation skips the log-only DSP timestamp and ASP diagnostic snapshot.
The callback still performs synchronous addressed I2C; it is not a hard realtime
execution guarantee. No acknowledgement deadline has been shortened.

Each successful pair commit publishes `Cirrus_Playback_PrepareMs_left` and
`Cirrus_Playback_PrepareMs_right`. These measure monotonic elapsed time from
the endpoint's first preparation write through pair commit. They include I2C
and callback delays, but exclude jack detection before preparation and audio
buffer latency after unmute. Compare repeated cycles with the same workload,
volume, firmware and boot options; report distributions, not one fastest sample.

## Provider review

The [custom I2C provider](i2c_provider.md) exports addressed combined transfers. DeviceNub interrupt APIs forward to its GPIO pin or ACPI
interrupt. The target snapshot declares GPIO pin 84, GPIO IRQ mode 3. Controller
power changes serialize against the I2C bus lock, disable the bus and its controller
interrupt during suspend, and reinitialize on wake.

This verifies API ownership, not amp interrupt delivery. The current amp setup
masks IRQ1/IRQ2. A complete amp IRQ path additionally needs verified GPIO2 pad
function and polarity, paired-source dispatch, acknowledgement, and safe suspend
ordering. GPIO1 drives the existing external-supply sequence and must not be
repurposed as an interrupt. Linux hibernate also depends on boost topology and
DSP wake/retry semantics; copying only its mailbox command is insufficient.
`-cirrusirq` enables a reference-only shared protection IRQ path with polling
backup. It requires GPIO mode, pin 84 and IRQ type 3. It applies the captured
GPIO2 open-drain function and protection mask, preserves GPIO1, restores polling
masks on setup failure, disables delivery before PM/teardown, and reconfigures on
wake. A protection fault latches and cleans up both endpoints without releasing
the protection source. An unexplained shared interrupt disables delivery instead
of spinning on an asserted level. Default boot keeps polling until this new path
has target delivery/sleep validation.

The local Linux capture (`CS35L41_AIO`) shows both amps sharing GPIO84, GPIO pad
`0x02000000`, GPIO2 control `0x81000001`, and IRQ1 mask `0x7FFD7E3F`. Its IRQ
counter is zero, so registration is supported but observed fault delivery is not.
Runtime hibernate still needs implementation and target wake validation; it is
not implemented.

## Regression coverage

Runtime tests execute production callback bodies and the actual driver.
They cover each cancellation phase, independent left/right register maps,
a delayed right endpoint, a failed second commit, elapsed PUP/PDN deadlines,
late I2C mailbox responses, unsupported-model rejection and idle without I2C.
Single-transfer fault sweeps cover start and asynchronous runtime cleanup.
The existing firmware, PM, calibration and protection suites remain required.

Host mocks cannot verify bus latency, speaker excursion, acoustic response,
interrupt delivery or whether the IOAudioFamily hooks load on target macOS.
