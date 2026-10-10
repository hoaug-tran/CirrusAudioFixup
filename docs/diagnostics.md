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
