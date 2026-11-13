# Codec and speaker route verification

Internal laptop speakers driven by Cirrus smart amplifiers depend on two distinct components working together:

1. **AppleHDA & AppleALC**: Route decoded PCM audio through the onboard HDA codec to its line/speaker output pins.
2. **CirrusAudioFixup**: Configures the external amplifiers over I2C or SPI, uploads DSP firmware, and unmutes the physical speakers.

If AppleALC routes audio to the wrong codec pins, the amplifiers will produce no sound even when initialized successfully (`READY_DSP`).

---

## 1. Check your current AppleALC layout-id

In macOS Terminal, run:

```bash
# Check the active layout-id on the audio controller
ioreg -lw0 -p IODeviceTree | grep -E 'layout-id|"alc-layout-id"'
```

The output displays a 4-byte little-endian hexadecimal value (e.g., `<15 00 00 00>` corresponds to layout ID `21` / `0x15`).

You can also check if an `alcid` boot argument is currently active:

```bash
nvram 7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args | grep -o 'alcid=[0-9]*'
```

---

## 2. Verify your layout-id

Your AppleALC `layout-id` is **correct** for Cirrus smart amplifiers when:

1. **Internal Speakers** appears in **System Settings → Sound → Output**.
2. Audio plays through the laptop speakers (not just headphones).
3. `ioreg -lw0 -p IOService -r -c CirrusAudioFixup` shows:
    - `Cirrus_Driver_Verdict` = `READY_DSP`
    - `Cirrus_Playback_Verdict_left` (or `_right`) = `ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED`

> [!WARNING]
> **Headphones work but Internal Speakers is silent?**
> This is a classic symptom of an incorrect `layout-id`. The current layout routes audio to the 3.5 mm headphone jack, but connects the speaker path to an unused pin.
>
> **Solution**: Test alternative layout IDs supported by your codec model by adding `alcid=<id>` to OpenCore boot-args (e.g., `alcid=11`, `alcid=13`, `alcid=21`). See the [AppleALC Supported Codecs list](https://github.com/acidanthera/AppleALC/wiki/Supported-codecs).

---

## 3. Extract the Linux codec dump

If none of the existing AppleALC layout IDs route audio to the internal speakers, extract the physical codec connections from Linux to create or patch an AppleALC layout:

Boot any live Linux USB distribution with working sound, then run:

```bash
mkdir -p ~/codec-dump
for codec in /proc/asound/card*/codec\#*; do
    [ -f "$codec" ] || continue
    card=$(basename "$(dirname "$codec")")
    sudo cat "$codec" > ~/codec-dump/"${card}-$(basename "$codec").txt"
done

# List ALSA audio playback devices
aplay -l > ~/codec-dump/alsa-devices.txt
```

Find the onboard audio codec header:

```bash
grep -HE '^(Codec:|Vendor Id:|Subsystem Id:|Revision Id:)' ~/codec-dump/card*-codec\#*.txt
```

> [!TIP]
> Keep two dumps if possible: one at idle and one while actively playing music through the internal speakers.

---

## 4. Locate AppleALC resources

In the [AppleALC repository](https://github.com/acidanthera/AppleALC/tree/master/Resources), open the directory corresponding to your codec model (e.g., `Resources/ALC285/` or `Resources/ALC287/`).

Examine the following files:

| File             | What to Inspect                                                      |
| :--------------- | :------------------------------------------------------------------- |
| `Info.plist`     | Layout IDs mapped to your codec subsystem ID.                        |
| `layout*.xml`    | Output channel definitions and `PathMapRef` IDs.                     |
| `Platforms*.xml` | Ordered signal path nodes (`NodeID`), connections, and amp settings. |

---

## 5. Trace the signal path

Trace the signal chain backwards from the amplifier pin to the DAC converter:

```mermaid
flowchart LR
    A[Audio Output Converter<br/>Node 0x..] --> B[Audio Selector / Mixer<br/>Node 0x..]
    B --> C[Pin Complex<br/>Node 0x..]
    C --> D[Cirrus Amplifiers<br/>I2S / ASP]
```

Compare the nodes found in your Linux dump against AppleALC:

| Component                 | Linux Codec Dump Marker                        | AppleALC XML Field                    |
| :------------------------ | :--------------------------------------------- | :------------------------------------ |
| **Output Pin**            | `Node 0x.. [Pin Complex]`                      | Pin node connected to amplifier input |
| **Intermediate Controls** | `Node 0x.. [Audio Selector]` / `[Audio Mixer]` | Intermediate `NodeID` in path map     |
| **DAC Converter**         | `Node 0x.. [Audio Output]`                     | Primary converter `NodeID`            |
| **Connection Selection**  | `Connection: 2` with `*` marker                | Active path selection                 |

> [!NOTE]
> Linux codec dumps report node IDs in **hexadecimal** (e.g., `0x17`), while AppleALC XML files use **decimal** integers (e.g., `23`). Make sure to convert between hex and decimal when verifying nodes.

---

## 6. Verify in macOS

Once AppleALC and CirrusAudioFixup are installed:

```bash
# Check detected audio devices
system_profiler SPAudioDataType

# Verify AppleHDA controller attachment
ioreg -lw0 -p IOService -r -c AppleHDAController

# Verify Cirrus amplifier status
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
```

1. Select **Internal Speakers** in macOS Sound Settings.
2. Test audio playback at low volume. Both left and right channels should produce clear sound without distortion.

---

## 7. Diagnosing multi-speaker and missing bass topologies

Many modern laptops (ASUS ROG, Lenovo Legion/ThinkBook, Dell XPS) feature quad-speaker setups composed of:

1. **Tweeters (high/mid frequencies)**: Driven directly by the onboard HDA codec via an analog Pin Complex (typically Node `0x14`).
2. **Woofers (bass frequencies)**: Driven by external Cirrus smart amplifiers (CS35L41 or CS35L56) receiving digital audio over I2S/ASP from a secondary Pin Complex (typically Node `0x17`).

### The missing bass problem

When using generic AppleALC layouts, `Platforms.xml` often only routes a single Pin Complex (the tweeters) to the active DAC converter. While `CirrusAudioFixup` initializes the external amplifiers into `READY_DSP` and enters `PLAYING` state:

- The secondary woofer Pin Complex remains in low-power state (`D3`), disconnected from the active DAC, or muted by the codec.
- Consequently, the laptop produces thin, treble-only sound without bass, as the smart amplifiers receive a silent digital stream.

### Diagnostic procedure

To resolve missing bass or silent amplifiers on any laptop:

1. **Identify the active DAC in macOS**:
   From your current working layout (e.g. `Platforms21.xml` or `Platforms88.xml`), identify which DAC Node (typically Node `0x02` or `0x03`) receives the primary audio output stream.
2. **Inspect the Linux codec dump for all speaker pins**:
   Search for all widgets marked `[Pin Complex]` with `Pin Default` set to `[Fixed] Speaker`:
    - Note the Node IDs (e.g. Node `0x14` and Node `0x17`).
    - Check the `Connection:` list for each pin to see which DAC converters it can select.
    - Note the index (0, 1, 2...) corresponding to the active DAC identified in step 1.
3. **Check pincap capabilities (EAPD)**:
   Inspect `Pincap` for each speaker pin:
    - If `EAPD` is present (e.g. `0x00010014`), EAPD verb `0x70C 0x02` is required.
    - If `EAPD` is absent (e.g. `0x0000001c`), EAPD verbs have no hardware effect and should be omitted.
4. **Construct the universal HDA activation sequence**:
   To test and activate an unrouted speaker pin at runtime using `alc-verb`, construct the standard 4-to-5 verb sequence:

    | Step                      | HDA verb | Parameter     | Purpose                                                          |
    | :------------------------ | :------- | :------------ | :--------------------------------------------------------------- |
    | **1. Connection select**  | `0x701`  | `<DAC Index>` | Connect the pin complex to the active DAC output stream          |
    | **2. Power state**        | `0x705`  | `0x00`        | Wake the pin complex to power state D0 (full power)              |
    | **3. Amplifier unmute**   | `0x3B0`  | `0x00`        | Unmute both left and right output amp channels at 0 dB gain      |
    | **4. EAPD enable**        | `0x70C`  | `0x02`        | Assert external amplifier power gate (_only if pincap has EAPD_) |
    | **5. Pin widget control** | `0x707`  | `0x40`        | Assert OUT enable to start audio output                          |

### Making the route persistent

Because `alc-verb` commands are runtime-only and reset across reboots or sleep/wake cycles, persist the sequence using one of two methods:

- **Method A (Native AppleALC layout)**: In `Platforms.xml`, add both speaker Pin Complexes into the same PathMap connected to the active DAC. Include any required initialization verbs in `ConfigData` and `WakeConfigData`.
- **Method B (Automated daemon)**: Install `ALCPlugFix-Swift` or a launchd script to dispatch the verified `alc-verb` sequence on boot and system wake events.

---

## 8. Verified hardware case studies and verb values

The following table summarizes real-world hardware where this diagnostic methodology successfully verified speaker and woofer playback:

| Laptop model                                         | Codec  | Amplifier        | Tweeter pin | Woofer pin  | Active DAC  | Verified verb sequence                                                     | Status         |
| :--------------------------------------------------- | :----- | :--------------- | :---------- | :---------- | :---------- | :------------------------------------------------------------------------- | :------------- |
| **Alienware x17 R2 / Dell Horizon 17** (`1028:0B28`) | ALC289 | 2x CS35L41 (SPI) | Node `0x14` | Node `0x17` | Node `0x02` | `alcid=93` (or Layout 87 + `01770100 01770500 0173b000 01770c02 01770740`) | Verified (#12) |
| **ASUS ROG Zephyrus G16 GU605MV** (`1043:1C63`)      | ALC285 | 2x CS35L56 (SPI) | Node `0x14` | Node `0x17` | Node `0x02` | `01770100 01770500 0173b000 01770c02 01770740`                             | Verified (#8)  |
| **Lenovo ThinkBook 16p G6 IAX** (`17AA:391E`)        | ALC287 | 2x CS35L56 (I2C) | Node `0x14` | Node `0x17` | Node `0x02` | `01770100 01770500 0173b000 01770740`                                      | Verified (#16) |
| **ASUS ROG Zephyrus G14 GA403UV** (`1043:1B13`)      | ALC285 | 2x CS35L56 (I2C) | Node `0x14` | Node `0x17` | Node `0x03` | `alcid=33` (or `01770c02 01770740`)                                        | Verified (#18) |
| **HP ENVY x360 15-ey0xxx** (`103C:8A31`)             | ALC245 | 2x CS35L41 (I2C) | N/A (dual)  | Node `0x17` | Node `0x03` | Routed via Layout 13 (`01770740`, no EAPD)                                 | Verified (#14) |
| **Lenovo Legion Slim 7 Gen 7** (`17AA:3877`)          | ALC287 | 2x CS35L41 (I2C) | Node `0x14` (dual) | N/A         | Node `0x02` | AppleALC Layout 21 (`alcid=21`)                                             | Verified (#21) |
| **ASUS ROG Strix G17 G713P** (`1043:1D1F`)            | ALC294 | 2x CS35L41 (I2C) | Node `0x17` (dual) | N/A         | Node `0x03` | AppleALC Layout 13 (`alcid=13`), no extra verbs; 48 kHz required          | Verified (#22) |

### Machine notes

#### Alienware x17 R2 / Dell Horizon 17 (`1028:0B28`, Realtek ALC289)

- **Problem**: Tweeters play normally on Node `0x14`, but woofers on 2x CS35L41 over SPI are completely silent. Driver logs `Cirrus_Diag_LatestFailure_spi0 = POWER_UP_TIMEOUT` with `0x10010` actual `0x400000` (`CLK_ERR`), expected `0x1000000` (`PUP_DONE`), or `PLL_UNLOCKED`.
- **Root cause**:
    1. The Alienware x17 R2 features 4 discrete physical speakers: 2 tweeters driven by the ALC289 integrated analog amplifier (Node `0x14`), and 2 woofers driven by dual mono CS35L41 amplifiers receiving digital audio over I2S from Node `0x17`.
    2. AppleALC Layout 87 (`alcid=87`) only configures Node `0x14` (tweeters) and completely omits Node `0x17` from `PinConfigurations` and `Platforms87.xml`. AppleHDA therefore never starts or clocks Node `0x17`. The missing BCLK causes CS35L41 to fail PLL locking (`PLL_UNLOCKED`), exhausting the driver's safety recovery attempts during early boot and latching `playbackFaulted`.
    3. AppleALC Layout 13 (`alcid=13`) is an incompatible XPS 9500 layout that omits Node `0x14`, disables the internal microphone (Node `0x12`), and attempts a 4-channel stream format (`0x0033`) that fails with `HDA_STREAM_FORMAT`.
- **Solution**:
    1. **Recommended (Zero-Configuration)**: Use AppleALC Layout 93 (`alcid=93`). Layout 93 defines both Node `0x14` (`0x90170140`) and Node `0x17` (`0x90112160`) as internal speakers with EAPD enabled (`0x70c 0x02`) and routes both in parallel from DAC `0x02` in `Platforms93.xml` (`0x14 -> 0x02, 0x17 -> 0x02, 0x21 -> 0x03`). BCLK is present from early boot, locking the PLL on the first attempt, while internal mic (Node `0x12`) and headset mic (Node `0x19`) remain fully functional on ADC `0x08`.
    2. **Alternative (Layout 87 + Verbs)**: If retaining Layout 87, send the 5-verb activation sequence to Node `0x17` during early boot before the driver's PLL recovery timeout engages:
        ```bash
        alc-verb 0x17 0x701 0x00   # Connect Node 0x17 to DAC 0x02
        alc-verb 0x17 0x705 0x00   # Wake Pin Complex to D0
        alc-verb 0x17 0x3b0 0x00   # Unmute amp at 0 dB gain
        alc-verb 0x17 0x70c 0x02   # Enable EAPD external amplifier gate
        alc-verb 0x17 0x707 0x40   # Pin widget control: enable OUT to start BCLK/I2S
        ```

#### Lenovo ThinkBook 16p G6 IAX (`17AA:391E`, Realtek ALC287)

- **Problem**: With AppleALC Layout 21 (`alcid=21`), tweeters played on Node `0x14` but woofers on 2x CS35L56 were silent.
- **Root cause**: Node `0x17` defaults to connection index 1 (DAC `0x03`), which carries no audio stream.
- **Solution**: Executed `01770100` (select DAC `0x02`), `01770500` (power D0), `0173b000` (unmute), and `01770740` (pin OUT). Node `0x17` pincap (`0x0000001c`) lacks EAPD, so `01770c02` was omitted. Both woofers produced clean, balanced bass immediately upon applying `01770740`.

#### ASUS ROG Zephyrus G16 GU605MV (`1043:1C63`, Realtek ALC285)

- **Problem**: With AppleALC Layout 88, only codec-driven tweeters produced audio.
- **Root cause**: Node `0x17` leading to CS35L56 SPI amplifiers was left unconfigured in layout 88.
- **Solution**: Applied 5-verb sequence `01770100 01770500 0173b000 01770c02 01770740`. Restored full-frequency bass playback matching Windows.

#### ASUS ROG Zephyrus G14 GA403UV (`1043:1B13`, Realtek ALC285)

- **Problem**: Only tweeters on Node `0x14` produced audio; woofers on 2x CS35L56 over I2C were silent.
- **Root cause**: The machine pairs an ALC285 codec with 2x CS35L56 amplifiers on I2C (`0x31` Left, `0x30` Right). Tweeters route from DAC Node `0x02` to Pin Complex Node `0x14`, while woofers route from DAC Node `0x03` to Pin Complex Node `0x17` via a 4-channel HDA stream format (`0x0033`).
- **Solution**: Use AppleALC Layout 33 (`alcid=33`), which defines paths for both Node `0x14` and Node `0x17`. If woofers remain silent on non-33 layouts, ensure Node `0x17` EAPD is enabled (`alc-verb 0x17 0x70c 0x02`) and pin widget control is asserted (`alc-verb 0x17 0x707 0x40`).

#### HP ENVY x360 15-ey0xxx (`103C:8A31`, Realtek ALC245)

- **Problem**: Internal speakers silent on generic layouts despite "Built-in Output" appearing in Sound Settings.
- **Root cause**: Node `0x17` pincap (`0x0000001c`) lacks an internal EAPD widget. Output depends entirely on CS35L41 receiving power over I2C.
- **Solution**: Layout 13 connects Node `0x17` to DAC `0x03`, allowing CS35L41 amplifiers to receive the PCM stream.

#### Lenovo Legion Slim 7 Gen 7 (`17AA:3877` / `17AA:385C`, Realtek ALC287)

- **AppleALC Layout**: Layout 21 (`alcid=21`) (or Layout 13 `alcid=13`). Note that inside macOS, AppleALC may dynamically map Layout 21 onto AppleHDA's internal layout index (e.g. Layout 7 on MacBookPro16,1 SMBIOS). Boot-args must specify `alcid=21`.
- **Speaker Pin**: Node `0x14` routed to DAC Node `0x02` (connection select `01470c02`). Node `0x17` is not routed on this model.
- **Amplifiers**: Dual CS35L41 (`CSC3551`) over I2C at addresses `0x40` (Left) and `0x41` (Right).
- **Topology**: External boost with VSPK switch (`ExternalWithSwitch`), active-low GPIO reset (`resetResource = 0`) and ACPI D0 (`_PS0`) power transition.
- **SSID Notes**: ACPI `_SUB` evaluates to `17AA:3877`, while audio PCI controller reports subsystem `17AA:385C`. CirrusAudioFixup automatically maps both to the verified embedded speaker protection tuning.

#### ASUS ROG Strix G17 G713P (`1043:1D1F`, Realtek ALC294)

- **AppleALC Layout**: Layout 13 (`alcid=13` or `layout-id` 13 in DeviceProperties). No extra `alc-verb` sequences are required.
- **Speaker Pin**: Node `0x17` routed to DAC Node `0x03`.
- **Amplifiers**: Dual CS35L41 (`CSC3551`) over AMD I2C (`AMDI0010`) at `0x40` (Left) and `0x41` (Right), IRQ polling.
- **Topology**: Internal boost, 1000 nH / 4500 mA / 24 uF. Uses the G733 (`1043:1503`) speaker protection tuning.
- **Stream format**: 48 kHz is required. With the built-in output at 44.1 kHz the stream is rejected (`UNSUPPORTED_STREAM_FORMAT`) and the speakers stay silent; select 48 kHz in Audio MIDI Setup.

[Back to main guide](../../README.md#installation)
