# Check the codec and speaker route

AppleHDA and AppleALC configure the HDA codec and its audio outputs. CirrusAudioFixup controls the amplifiers attached to the speaker path. Both parts must work: amplifier initialization cannot replace a missing codec route, and a visible audio output does not prove that samples reach the speakers.

Use the codec connections from the same physical machine. Do not choose an audio configuration only because another laptop has the same codec model.

## 1. Save the Linux codec dump

Boot Linux with the machine's audio drivers loaded. These commands save the HDA codec descriptions without changing codec registers:

```bash
mkdir -p codec-dump

for codec in /proc/asound/card*/codec\#*; do
    [ -f "$codec" ] || continue
    card=$(basename "$(dirname "$codec")")
    sudo cat "$codec" > "codec-dump/${card}-$(basename "$codec").txt"
done

aplay -l > codec-dump/alsa-devices.txt
```

Keep the individual files: a machine can expose several codecs, including GPU HDMI/DisplayPort codecs. Inspect their headers to identify the onboard codec:

```bash
grep -HE '^(Codec:|Address:|Vendor Id:|Subsystem Id:|Revision Id:)' \
    codec-dump/card*-codec\#*.txt
```

If there are no matching files, stop and check the Linux audio-driver state. An empty dump is not a valid routing reference. `Tools/collect_audio_amp_linux.sh` can collect a broader audio, ACPI and controller baseline.

For a playback problem, keep separate captures at idle and while playing through the internal speakers. The selected connection and stream assignments can change with driver state. The [Linux HD-Audio notes](https://www.kernel.org/doc/html/latest/sound/hd-audio/notes.html) describe the codec dump and diagnostic interfaces.

## 2. Open the matching AppleALC source

Use the source revision or fork corresponding to the installed AppleALC build. A different revision can contain different resources.

Open [AppleALC Resources](https://github.com/acidanthera/AppleALC/tree/master/Resources), then the directory for the onboard codec, such as `Resources/ALCxxx/`. Inspect:

| File | What to check |
| --- | --- |
| `Info.plist` | Codec identity and the layout/platform resources selected by its file entries |
| `layout*.xml` or `layout*.xml.zlib` | Output definitions and `PathMapRef` references, including the speaker output |
| `Platforms*.xml` or `Platforms*.xml.zlib` | `PathMapID`, ordered path nodes, `NodeID` and channel/amp settings |

Follow the filenames and IDs in `Info.plist`; do not assume layout and platform filenames have the same numeric suffix. If the checkout contains only compressed resources, use AppleALC's `Tools/zlib_unpack.command` on a working copy to inspect the XML. See [AppleALC's codec resource guide](https://github.com/acidanthera/AppleALC/wiki/Adding-codec-support) for the file structure.

## 3. Compare the output path

Start at the codec pin feeding the speaker amplifier and trace its connection list back through any selector or mixer to the audio output converter. Compare that chain with the speaker path in the selected AppleALC resources.

| Linux codec dump | AppleALC comparison |
| --- | --- |
| `Vendor Id`, codec name and revision | Confirm the resources belong to the actual onboard codec |
| `Node 0x.. [Audio Output]` | Output converter used by the speaker path |
| `Node 0x.. [Audio Selector]` / `[Audio Mixer]` | Intermediate nodes and available connections |
| `Node 0x.. [Pin Complex]` | Output pin connected to the speaker/amp path |
| `Connection:` list and selected `*` entry, where present | Valid connections and current selection; account for the capture's playback state |
| Pin configuration, pin controls and amp capabilities | Output direction, pin setup and relevant mute/gain behavior |

Linux usually prints node IDs in hexadecimal; AppleALC XML often stores them as decimal integers. Convert the numbers before comparing them. The node sequence and channel assignments must describe a valid path on this machine. Not every node in a codec dump belongs in the speaker path.

The dump records the codec's capabilities and current state, including Linux driver fixups. It does not, by itself, prove the board's physical wiring, ASP/I2S clock delivery, amplifier tuning or the route active under macOS. Use board evidence and a working speaker-playback baseline to resolve those parts.

## 4. Verify the macOS output

```bash
system_profiler SPAudioDataType
ioreg -lw0 -p IOService -r -c AppleHDAController
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
```

Confirm that the intended output exists and is selected, AppleALC has configured the matching codec route, and the hardware output stream meets CirrusAudioFixup's supported format. Then inspect both amplifier endpoints and test left/right speaker output at low volume.

A loaded AppleALC kext, a running HDA stream, working headphones or a `READY_DSP` verdict each establishes only part of the chain. Successful internal speaker playback requires the codec route, audio clocks, amplifier configuration and channel tuning to agree.

[Back to installation](../README.md#installation)
