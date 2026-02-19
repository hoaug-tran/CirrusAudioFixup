# CirrusAudioFixup

Skeleton kext for **Cirrus Logic CS35L41** internal speaker amplifier on Hackintosh.

- **Target hardware**: Lenovo Legion 7 (2021) — CS35L41 via I2C (addr 0x40 / 0x41)
- **Minimum macOS**: Ventura 13.0
- **Bundle ID**: `com.hoaugtr.CirrusAudioFixup`
- **Provider**: `VoodooI2CDeviceNub` (requires VoodooI2C loaded first)
- **ACPI HID**: `CLSA0100` (devices SPL / SPR in custom SSDT)

---

## Project structure

```
CirrusAudioFixup/
├── .github/workflows/build.yml   ← GitHub Actions CI
├── CirrusAudioFixup.xcodeproj/   ← Xcode project
└── CirrusAudioFixup/
    ├── Info.plist                 ← Kext metadata & IOKit personalities
    ├── CirrusAudioFixup.hpp       ← Class declaration
    └── CirrusAudioFixup.cpp       ← Implementation
```

---

## Building

### Via GitHub Actions (recommended — no macOS required locally)

1. Push your changes to `main`  
2. Go to **Actions** tab on GitHub  
3. Wait ~3 minutes for build to finish  
4. Download **CirrusAudioFixup-{sha}.zip** from the Artifacts section  
5. Extract and place `CirrusAudioFixup.kext` into `EFI/OC/Kexts/`

### Locally (requires macOS + Xcode 15)

```bash
cd CirrusAudioFixup
xcodebuild \
  -project CirrusAudioFixup.xcodeproj \
  -target CirrusAudioFixup \
  -configuration Release \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  build
```

---

## OpenCore config

```
EFI/OC/Kexts/ (load order matters):
  1. VoodooI2CController.kext
  2. VoodooI2C.kext
  3. CirrusAudioFixup.kext   ← AFTER VoodooI2C
```

**SSDT required**: Custom SSDT that splits the ACPI speaker device into:
- `SPL` — HID=`CLSA0100`, UID=0, I2C addr 0x40 (Left)
- `SPR` — HID=`CLSA0100`, UID=1, I2C addr 0x41 (Right)
- GPIO 0x0006 (reset) declared **Exclusive only in SPL**

---

## Safety: boot-arg guard

The kext loads and matches but does **nothing** unless you add `cirrus=1` to boot-args.

```
# In OpenCore config.plist → Kernel → Boot → boot-args:
# For passive test (just verify matching):
# (no extra args needed)

# For active hardware init (future):
cirrus=1
```

This means you can always boot safely — even if the I2C code has bugs.

---

## Verifying it works (passive mode)

After boot, run in Terminal:

```bash
# Check if kext loaded
kextstat | grep -i cirrus

# Check IOLog output
log show --predicate 'process == "kernel" AND message CONTAINS "CirrusAudioFixup"' \
  --style compact --last 5m
```

Expected output:
```
[CirrusAudioFixup] init() — kext loaded and initialised
[CirrusAudioFixup] probe() — provider: CLSA0100 | score: ...
[CirrusAudioFixup] start() — BEGIN
[CirrusAudioFixup] detectChannel() — UID=0 → LEFT channel (I2C 0x40)
[CirrusAudioFixup] start() — boot-arg 'cirrus=1' NOT set → passive mode
[CirrusAudioFixup] start() — SUCCESS ✓ (LEFT channel matched)
```
