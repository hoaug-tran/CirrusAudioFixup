# Collect audio evidence on macOS

Run commands on the affected installation, from the repository root where a relative `Tools/` path is used. These commands inspect state; they do not install kexts, patch AppleHDA or change logging persistence. Use a Debug build or boot Release with `-cirrusdbg` when routine driver messages are needed.

## Service, loaded extensions and audio devices

```bash
/usr/bin/sw_vers
/usr/sbin/ioreg -lw0 -p IOService -r -c CirrusAudioFixup
sudo /usr/bin/kmutil showloaded
/usr/sbin/ioreg -lw0 -p IOService -r -c AppleHDAController
/usr/sbin/system_profiler SPAudioDataType
```

`sw_vers` identifies the OS build. The driver query should contain the service and its `Cirrus_*` properties when attached. No matching output means attachment has not been demonstrated. `kmutil` checks loaded extensions, including AppleHDA, AppleALC, Lilu and the I2C provider; inspect the full output before filtering. AppleALC is not expected to register an IOService class named `AppleALC`. CS35L41 endpoints are tracked by CirrusAudioFixup, not separate services named `CS35L41`.

The controller query and audio-device list establish different facts: finding the controller does not prove that the codec route is programmed or that PCM reaches the amplifier. On Tahoe, follow the [host-stack requirements](../README.md#tahoe-restore-and-validate-the-host-audio-stack-first) before diagnosing amplifier playback.

## Attachment layers and machine metadata

Read `Cirrus_Communication_Status`, `Cirrus_Amplifier_Status`,
`Cirrus_Attach_First_*`, `Cirrus_Attach_Latest_*` and typed index fields before
interpreting a missing speaker as an AMP problem. The first attach failure survives
later cleanup/PM failures. `COMMUNICATION`, `RESOURCE`, `AMPLIFIER`, `POLICY`,
`BINDING`, `DEBUG_POLICY` and `CONTROLLER_PM` identify the failed boundary.

`Cirrus_CPU_*`, `Cirrus_Machine_Model`, `Cirrus_Provider_*`, platform profile and
PCI subsystem identify the captured machine/provider. CPU vendor does not choose
an AMP, GPIO plan or controller. The collector also captures CPU/model metadata
for older loaded bundles that do not publish the new schema. Compare the loaded
UUID with the intended build; missing new fields may simply mean an older binary.

`ATTACH` and `ATTACH_FAIL` are essential records. Verbose provider enumeration
requires a Debug build or `-cirrusdbg`; phase acceptance is module-specific and
published in `Cirrus_Debug_Phase`. A failed `start()` may leave no service node,
so also inspect kernel failure records. See the [attachment matrix](attachment_matrix.md)
for Intel/AMD, I2C/SPI and unsupported-controller/AMP combinations.

## Retained logs and live messages

```bash
sudo /usr/bin/log show --last 1h --style syslog --info --debug \
  --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
sudo /usr/bin/log stream --level debug --style syslog \
  --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
```

The first command reads the retained last hour; the second watches new messages until Ctrl-C. An empty result can mean no matching messages were retained, routine logging was disabled, or the service did not attach. It does not mean playback passed. Including debug messages cannot recover messages already discarded. Apple's [logging documentation](https://developer.apple.com/documentation/os/viewing-log-messages) explains retained versus streamed messages. Inspect `log help show` and `log help stream` on the affected OS if options are rejected.

The collector requests logs since boot with `--last boot`. If that query fails, it preserves the error and separately retries the last hour in `kernel-log-fallback.txt`. An empty successful query does not trigger a retry. Kernel-buffer fallbacks are:

```bash
sudo /sbin/dmesg
/usr/sbin/sysctl -n kern.msgbuf
```

Availability, permissions and retained buffer contents vary; neither is guaranteed to contain the whole boot. The collector records errors rather than treating a missing buffer as a driver fault.

## Save a report before rebooting

```bash
sudo bash Tools/collect_macos.sh
```

It prints an evidence directory, not an archive. Read `status.txt` and the raw files: an exit status of zero from `ioreg` still permits no matching service. Some optional NVRAM properties, registry planes or developer tools may be absent. Review boot arguments, calibration data and hardware identifiers before sharing. The directory is temporary; copy it to a persistent location before rebooting.

To also identify the bundle being tested, pass its actual path as the sole argument. For a local Release build:

```bash
sudo bash Tools/collect_macos.sh "$PWD/build/Release/CirrusAudioFixup.kext"
```

This inspects that file; it does not prove the loaded extension is the same binary. Compare its version/build metadata with the service and your OpenCore entry. `dwarfdump` and `nm` require developer tools and may fail on a machine without them. An unsigned OpenCore bundle may have no code signature; that observation alone is not a load failure.

## First microphone activation causes a speaker pop

Treat this as a reported transition problem, not a confirmed layout-id or amplifier defect. The driver observes HDA output descriptors, not microphone widgets. Opening an input can coincide with a codec power, route or clock transition; the current evidence does not identify which component produces the pop.

Keep the OS, layout, firmware and volume unchanged during a comparison. On a known working board, with low speaker volume:

1. After idle, save a collector report before opening the input pane. Record whether playback is stopped or running.
2. In another Terminal, start a short live capture:

   ```bash
   sudo /usr/bin/log stream --level debug --style syslog \
     --predicate 'process == "kernel"' > mic-transition.log 2>&1
   ```

3. Record the time with `date '+%Y-%m-%d %H:%M:%S %z'`, then open Sound input settings (System Preferences on older macOS, System Settings on newer releases). Note the first pop and whether immediate reopening repeats it. Stop the stream with Ctrl-C and collect a second report.
4. If the pop is quiet and testing is safe, compare after idle and after sleep/wake, then stopped versus quiet playback. Stop if the pop is loud or repeats unexpectedly.

Compare `Cirrus_Audio_Event_Mode`, `Cirrus_HDA_Status`, stream metadata, both playback verdicts and first/latest failures. `IOAUDIO_FAMILY_EVENTS` identifies a recognized built-in output selector. Headphone and stopped-engine states do not schedule periodic amplifier monitoring in this mode. `HDA_TIMER_FALLBACK` and `IOAUDIO_HOOKS_WAITING_FOR_OUTPUT` still use stream observation and may miss short changes. A new output activation or PLL/power fault would justify investigating the driver transition. Do not change multiple layouts, gains and firmware images at once. Do not unload a running kernel driver to create an A/B test.

## Louder sound, EQ and DSP bypass

Ask what “without it” means: without this kext, without DSP, without a Windows enhancement, or without an EQ app. These are different comparisons. Record the track, output device, OS volume, firmware/tuning identity and any active effects.

Louder output does not establish better frequency response or correct protection. Firmware and channel coefficients can affect DSP behavior; Windows Lenovo/Nahimic processing is a separate part of the audio chain and is not reproduced merely by uploading amplifier firmware. This driver does not reproduce that user-space effects stack.

Software EQ cannot substitute for verified boost settings, measured calibration or amplifier protection. `-cirrusnodsp` skips firmware and tuning and removes DSP acoustic protection. Do not use a long or high-volume bypass test to assess sound quality. Gain registers differ between the paths; their raw hexadecimal values are not comparable loudness measurements. Acoustic comparison requires matched output levels and suitable measurements, not just the same OS volume slider.

## Playback preparation timing

After each successful stereo commit, IORegistry publishes
`Cirrus_Playback_PrepareMs_left` and `Cirrus_Playback_PrepareMs_right`. Query the
service after each speaker start or headphone removal. Values include the
endpoint's preparation and pair commit; they do not measure the entire audible
jack-to-speaker delay. Keep volume/workload fixed and compare repeated cycles.
Release without `-cirrusdbg` skips log-only I2C reads.

`-cirrusirq` is an experimental reference-board protection delivery option. It
requires the captured GPIO84/type3 provider. `Cirrus_Amp_IRQ_Mode` reports setup,
fallback or latched-fault state. Polling health remains enabled. Registration is
not proof that interrupts were delivered; do not manufacture a speaker short or
overtemperature to test it. IRQ is disabled by default pending target validation.

## Diagnostic readiness audit (2026-10-12)

Debug defaults to verbose logging; Release enables it with `-cirrusdbg`.
`-cirrusdumptrace` explicitly enables transfer history even without verbosity.
Quiet Release does not allocate a trace lock, timestamp or lock each transfer,
serialize transfer history, or perform failure-snapshot register reads. Essential
failure reporting and all safety/protection/readback checks remain enabled.
`Cirrus_Trace_Enabled` distinguishes disabled history from a zero-transfer capture.
Trace allocation failure reports `DIAGNOSTICS_UNAVAILABLE` and does not prevent
otherwise safe startup. A history is bounded to the newest 1,023 transfers.

| Boundary | Evidence to inspect | Interpretation / limit |
| --- | --- | --- |
| Loading / dependencies | Collector loaded bundle UUID, build metadata, kernel load errors | An absent service may mean the bundle never loaded; AMP diagnostics cannot explain a pre-load failure |
| Attachment / board | `ATTACH`, CPU/provider/path, subsystem IDs, attachment first/latest stage/reason/return/index kind | CPU vendor is metadata; board authorization and communication availability are separate |
| Resources / communication | Bus classification, controller/native return, identity register and raw ID, `TRACE_PROBE` | Distinguishes absent ABI, invalid resources, failed transfer and readable wrong silicon; cannot prove the electrical cause of a timeout |
| AMP initialization / firmware | Runtime first failure stage/return/cleanup return, diagnostic first/latest, firmware identity and chip stage | Missing/late identity is bounded waiting; unsupported identity and parse/upload faults do not trigger guessed firmware |
| Playback / host audio | Route/event mode, HDA stream observation, phase deadlines, PLL/protection/mailbox failure evidence | READY_DSP and HDA RUN alone do not prove audible output |
| IRQ / fallback | IRQ mode and fallback reason; configuration native return | Missing/failed IRQ retains polling; no unverified wiring is enabled |
| PM / teardown | `Cirrus_PM_Registration_Return`, controller PM attachment evidence, runtime cleanup status | Registration errors log the native return before service stop; controller resume failure preserves its first cause |
| Diagnostics unavailable | `DIAGNOSTICS_UNAVAILABLE`, `Cirrus_Trace_Enabled` | Missing trace after allocation failure is not evidence of zero hardware traffic |

For each AMP, immutable root evidence is published as
`Cirrus_Diag_FirstStage_<name>`, `FirstReg`, `FirstExpected`, `FirstActual` and
`FirstIOReturn` under the same `Cirrus_Diag_` prefix. Existing failure fields
continue to describe the latest error. A cleanup failure cannot overwrite the
root transfer return or register. The failure counter saturates rather than
wrapping. Repeated identical failure publications remain rate limited.

In verbose mode, first/latest trace text is frozen **before** failure-snapshot
register reads. The latter must not displace the transfer that caused the fault.
Trace entries use `AMP<endpoint index>` rather than guessing left/right from
index or transport. Trace formatting, allocation and IORegistry publication
occur outside the recorder lock, using a bounded copied history; text construction
is linear in the number of captured bytes.

This is software diagnostic readiness, not a guarantee that every physical root
cause is observable. NACK/timeouts may still require controller logs, ACPI/resource
captures, electrical measurements or a known-good comparison. Source fault tests
cover native errors and cleanup; cold boot, output routing, IRQ delivery, repeated
sleep/wake and performance under real logging load remain hardware checks. Debug
logging can perturb timing, so compare with quiet Release after collecting evidence.

`Cirrus_Offline_Return_Count` and `Cirrus_Timeout_Return_Count` count observed
transfer returns. The legacy `Cirrus_NOACK_Count` remains an alias for Offline
returns for existing collectors; it is not proof of an electrical NACK. Internal
legacy `retries` counts Timeout returns, not measured controller retries.

The first transfer failure freezes trace history even when invasive snapshots are
not requested. A later chip-level snapshot can update latest history without
replacing that first trace. The maintained test executes both nested reporting
steps explicitly.

Research reference: the upstream [Linux dynamic-debug documentation](https://www.kernel.org/doc/html/v6.12/admin-guide/dynamic-debug-howto.html)
describes explicit activation of debug call sites. This audit applies the same
policy distinction to the existing macOS driver verbosity switch; it does not
claim Linux jump-label patching or zero instruction overhead on macOS.
