# CirrusAudioFixup

Debug kext for **Cirrus Logic CS35L41** speaker amps on Hackintosh.

This kext does not enable speakers yet. It collects safe debug data needed to build the real driver.

## Target

- Laptop: Lenovo Legion 7 2021
- Codec: Realtek ALC287
- Amps: Cirrus Logic CS35L41
- I2C addresses: `0x40` and `0x41`
- Expected device id: `0x00035A40`
- Expected revision: `0x000000B2`

## Required VoodooI2C patch

`CirrusAudioFixup` needs a custom VoodooI2C build with this platform function:

```text
VoodooI2CTransferToAddress
```

VoodooI2C stays the bus owner. This kext only asks VoodooI2C to run debug I2C transfers.

## Boot args

Passive mode:

```text
(no boot arg)
```

Read-only probe mode:

```text
cirrus_probe=1
```

Probe mode waits 10 seconds, then reads:

```text
0x40 reg 0x00000000
0x40 reg 0x00000004
0x41 reg 0x00000000
0x41 reg 0x00000004
```

No reset, no firmware upload, no boost config, no unmute.

## OpenCore load order

```text
1. VoodooI2CController.kext
2. VoodooI2C.kext       (custom patched build)
3. CirrusAudioFixup.kext
```

## Logs

```bash
log show --predicate 'process == "kernel" AND message CONTAINS "CirrusAudioFixup"' \
  --style compact --last 5m
```

Good result:

```text
[CirrusAudioFixup] amp left probe address=0x40
[CirrusAudioFixup] read amp=left addr=0x40 reg=0x00000000 value=0x00035A40
[CirrusAudioFixup] read amp=left addr=0x40 reg=0x00000004 value=0x000000B2
[CirrusAudioFixup] amp right probe address=0x41
[CirrusAudioFixup] read amp=right addr=0x41 reg=0x00000000 value=0x00035A40
[CirrusAudioFixup] read amp=right addr=0x41 reg=0x00000004 value=0x000000B2
```

## Safety

This is debug-only code. Speaker output needs more work:

1. safe reset handling
2. CS35L41 config sequence
3. DSP firmware loader
4. left/right tuning data
5. AppleALC speaker route

Do not replay raw Linux I2C traces in one loop.
