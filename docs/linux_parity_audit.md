# CS35L41 parity audit

Source review date: 2026-10-10. This is a source and host-test audit, not an
acoustic measurement or macOS hardware certification.

## Corrections in this change

| Area | Finding | Correction |
| --- | --- | --- |
| Software reset | The reset register and command were legacy values, not the Linux CS35L41 definitions | Use register `0x20` and command `0x5A000000`; keep DEVID at `0x00` |
| Digital unmute | Writing zero disabled the PCM high-pass filter | Use `0x8000`: high-pass enabled, digital volume 0 dB |
| Stereo preparation | One active endpoint selected the 100 ms health timer even with another endpoint awaiting preparation | Keep eligible pending endpoints on the bounded 5 ms preparation timer |
| Quiet Release | Routine snapshots read registers even when their logs were disabled | Skip log-only snapshots without verbosity; preserve mandatory safety readbacks |
| Discovery dumps | Normal Release discovery performed an unsolicited full register sweep | Restrict sweeps to verbosity, read-only mode or the probe phase |
| Event subscription | The attached flag was published under a different lock from its readers | Publish under the mailbox lock; repeated attachment is idempotent |
| Initial event state | Registry seeding could overwrite a newer completed output observation | Ignore seed data once a recognized route has been published |
| Register regression | Alias equality could preserve an incorrect copied value; a failed executable did not fail the script | Add independent Linux policy assertions and propagate executable failure |

## Loudness and sound quality

The DSP gain defaults to PCM code 17 and PDM code 19. The PCM gain is 17.5 dB;
the encoded register is `0x233`. Bypass uses `0x84`, with PCM gain 4.5 dB.
These match Linux HDA defaults. Neither register sets the macOS volume-slider
curve. Unmuted digital amplifier volume is 0 dB, not a deliberate attenuation.

This does not prove that the speaker is playing at its intended acoustic level.
The host codec's volume controls, DSP coefficients, calibration, source material
and any user-space processing remain separate parts of the signal path.
Increasing amplifier gain changes maximum output as well as middle-slider output.
It must not substitute for checking the intended board tuning.

Linux can override its default gain using a companion `.bincfg` tuning file.
This checkout now imports optional companions into the executable and parses them
per channel before firmware upload. The existing reference entry has no companion;
its gain remains the Linux default. Malformed embedded companions reject resource
selection. Import and runtime parsers reject duplicate gains, truncation, invalid
sizes and out-of-range PCM codes. Missing right-channel data does not inherit left
data. Matching WMFW/BIN container
formats and successful uploads do not establish matching tuning gain or EQ.
Measured calibration is optional here and can be unavailable; DSP readiness is
not proof of measured calibration.

The [Windows reference audit](windows_driver_audit.md) establishes exact OEM
firmware/coefficient matches for the reference Veco profile and a matching
configured 17.5 dB gain. There is no evidence that reproduces the Windows OEM effects
chain, its volume curve or its acoustic response. Windows equivalence remains
unverified. No normal/bypass comparison should be made at an assumed equivalent
slider position; acoustic levels need to be matched first.

## Coverage and remaining differences

| Area | Present behavior | Boundary or remaining work |
| --- | --- | --- |
| Output changes | IOAudioFamily selector and engine hooks wake the amplifier workloop | Actual hook loading is unverified; unknown routes or unavailable hooks use explicit polling fallback |
| Headphone idle | Recognized headphone/stopped-engine state leaves no periodic monitor timer after cleanup | Cleanup may need bounded retries; active speaker health still uses a timer |
| Power transitions | Runtime start/stop advance through phases with monotonic acknowledgement deadlines | I2C, initialization and PM quiesce remain synchronous; no measured end-to-end latency bound |
| Route cancellation during waits | New generations cancel preparation at each callback and before/after pair commit | Cancellation cannot interrupt a provider transfer already in progress |
| Stereo | Both endpoints prepare muted; pair readiness gates commit, with whole-pair rollback | Register writes remain sequential; sample-synchronous unmute is not guaranteed |
| DSP | Firmware parsing, upload/readback, mailbox and HALO checks | Firmware provenance and per-board acoustic behavior are not certified by these checks |
| Protection | Required protection checks and latched faults stay active in Debug and Release | Hardware IRQ delivery and Linux automatic protection-release behavior are not fully reproduced |
| Power management | Serialized sleep/wake cleanup and software reinitialization | Linux runtime autosuspend, hibernate and regcache restoration are not equivalent implementations here |
| Hardware support | CS35L41 stereo profile with initialization and runtime model gates; playback sequences live in its backend | Firmware lifecycle remains service-owned; other amplifiers still require complete backends and validation |
| Formats | 48 kHz stereo format gate | Broader sample rates and channel configurations are not supported |
| Packaging | Pinned SDK/Lilu, Debug/Release policy, metadata and symbol checks in CI | Local Windows object compilation does not establish a linked/loadable bundle |

The reported first microphone-activation pop is not traced to a verified source.
Changing output-amplifier gain or codec power verbs would not be an evidence-based
fix for it.

## Validation

Host regressions exercise production function bodies with mocked hardware.
They cover reset ordering, I2C faults, firmware bounds, calibration, power cleanup,
mailbox/PLL timeouts, output-event filtering and obsolete-transition cancellation.
The new tests also check high-pass-preserving unmute, quiet snapshots and pending
stereo preparation. They do not measure sound, temperature, excursion or latency.

Before describing the kext as equivalent to the reference driver, validate the
linked bundles and hook loading on target macOS. Acoustic parity additionally
requires the same board resources, applicable gain override and calibration,
then level-matched playback measurements. No additional logs are requested by
this source audit.

## Upstream references

- [Linux v6.12 CS35L41 register definitions](https://github.com/torvalds/linux/blob/v6.12/include/sound/cs35l41.h)
- [Linux v6.12 HDA gain defaults](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.h)
- [Linux v6.12 HDA playback and tuning gain](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.c)
- [Current Linux HDA implementation](https://github.com/torvalds/linux/blob/master/sound/hda/codecs/side-codecs/cs35l41_hda.c)
- [Linux v6.12 ASoC volume scale](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41.c)

The numbered Linux tag is the reproducible reference for the corrected constants.
The moving master reference was also inspected for current tuning-file behavior;
it is not a pinned parity target.
