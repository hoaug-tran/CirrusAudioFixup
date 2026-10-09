# Contributing to CirrusAudioFixup

Changes to this driver can affect amplifier power rails and speaker output. Keep a recoverable EFI and distinguish source checks from physical playback validation.

---

## General Principles

> [!IMPORTANT]
> **Boot & Hardware Safety First:**
> - Never submit unverified firmware, aggressive gain boosts, or experimental voltage changes without real hardware validation.
> - Do not broaden `Info.plist` matches or add placeholder entries without verified IORegistry dumps and an emergency rollback plan.
> - Keep pull requests focused on a single feature, bugfix, or hardware target.

---

## Adding Support for New Hardware

When submitting support for a new laptop model or amplifier configuration, please open a PR or Issue with the following details:

1. **Platform Details**: Laptop model name, CPU platform (e.g., AMD Cezanne / Intel Tiger Lake).
2. **Audio Hardware Identifiers**:
   - Audio Codec ID & Subsystem ID (SSID, e.g., `17AA:3847`).
   - ACPI Device Name under VoodooI2C (e.g., `CLSA0100` or `CSC3551`).
   - Amplifier model (e.g., Cirrus Logic CS35L41) and I2C target addresses (e.g., `0x40`, `0x41`).
   - AppleALC layout-id and tested audio path (Converter NID to Speaker Pin NID).
3. **Dumps & Evidence**:
   - ACPI table dump (`DSDT` / `SSDT`).
   - Linux ALSA codec dump (`/proc/asound/card*/codec#*`).
   - IORegistry dump containing active `Cirrus_*` properties.
   - Source of firmware (`.wmfw`) and channel tuning (`.bin`) from upstream `linux-firmware`, including resource identity. Tuning coefficients are not measured per-speaker calibration.
   - Exact macOS build, host audio stack and any AppleHDA restoration patch/version. VoodooHDA combinations remain untested unless accompanied by new evidence.

---

## Coding Standards & Kernel Constraints

### C++ & XNU Kernel Environment

- **Standard**: C++17.
- **Constraints**:
  - No C++ exceptions (`-fno-exceptions`).
  - No RTTI (`-fno-rtti`).
  - No STL containers in kernel code (`std::vector`, `std::map`, etc. are forbidden in kernel space; use IOKit collections or fixed-size structures).
  - Use `#pragma once` for all internal header files.
  - No raw `new` / `delete` outside of IOKit memory allocation helpers (`IOLockAlloc`, `IOMalloc`, `IOFree`).

### Code Style & Naming Conventions

Place comments above the statement or declaration they describe. Explain
ownership, thread context, hardware ordering and failure handling where those
details are not evident from the code. Avoid comments that merely repeat a name.
File headers should describe the file's responsibility; do not invent creation
dates or replace upstream attribution with project ownership.

| Element | Convention | Example |
| :--- | :--- | :--- |
| **Classes / Structs** | `PascalCase` | `AmplifierState`, `CS35L41Device` |
| **Methods / Functions** | `lowerCamelCase` | `synchronizeHdaStream()`, `stopPlayback()` |
| **Member Variables** | `m` + `PascalCase` | `mPowerAvailable`, `mProbeTimer` |
| **Constants / Enums** | `k` + `PascalCase` | `kMaxAmps`, `kRegPowerControl1` |
| **Namespaces** | `lowercase` | `cirrus::devices::cs35l41` |
| **Macros** | `UPPER_SNAKE_CASE` | `CIRRUS_LOG`, `CIRRUS_ERR` |

### Documentation & Comments

- Write clean, expressive, and self-documenting code.
- Use comments where they provide real engineering value: explaining non-obvious hardware quirks, silicon errata workarounds, timing constraints, or datasheet references.
- Avoid leaving dead, commented-out code blocks or trivial comments that merely repeat what the code does.

---

## Validation & Required Checks

Run from the repository root. Host tests need Python 3, Bash and a C++17 compiler named `g++`. Validate both Debug and Release; their logging defaults differ, but hardware checks must remain the same.

### 1. Run Host Python Test Suite

```bash
python3 Tests/reproduce_host.py
python3 Tests/check_registers.py
python3 Tests/check_transport.py
python3 Tests/check_diagnostics.py
python3 Tests/check_hda.py
python3 Tests/check_calibration.py
python3 Tests/check_bringup.py
python3 Tests/check_runtime.py
python3 Tests/check_architecture.py
python3 Tests/check_release.py
python3 Tests/check_build_policy.py
python3 Tests/check_tools.py
```
> [!NOTE]
> Tests include intentional fault-injection sweeps. Intermittent `ERROR` log messages in the test output are expected; the suite must finish with exit code `0` and print `PASS`.

### 2. Build on macOS

Prepare the pinned MacKernelSDK checkout using the [build instructions](README.md#building-and-testing). Do not replace a modified SDK checkout without review.

```bash
xcodebuild -project CirrusAudioFixup.xcodeproj \
           -target CirrusAudioFixup \
           -configuration Release \
           -sdk macosx \
           CODE_SIGNING_REQUIRED=NO \
           CODE_SIGN_IDENTITY="" \
           CODE_SIGNING_ALLOWED=NO \
           CONFIGURATION_BUILD_DIR="$PWD/build/Release" build
```

Repeat with `Debug` and `build/Debug`. Keep matching dSYMs for each binary; verify the binary and dSYM UUIDs before distributing them. A successful link still needs load, transport and hardware checks.

---

## Release validation

Publish only a new `vX.Y.Z` tag after Debug/Release builds and target-machine validation.
The tag, latest versioned changelog entry and both bundle version fields must agree.
Normal branch pushes produce CI artifacts, not public releases. Existing release assets
must not be overwritten. CI packages include commit/configuration metadata and release
checksums; successful host tests do not prove hardware playback or speaker safety.

Before tagging, test cold boot, playback start/stop, headphone switching and sleep/wake
on the supported machine. Collect IORegistry and unified logs with `Tools/collect_macos.sh`
and inspect both amplifier verdicts and first/latest failures. Keep a known-good fallback
EFI and begin hardware validation at low volume.

Include first microphone activation after idle/wake in transition tests. Use the [diagnostic guide](docs/diagnostics.md) to record timestamps and pre/post state. Do not label a pop as a layout defect without isolating its source. Report acoustic differences separately from register readiness and compare loudness at matched levels.

## License & Attribution

- This project is licensed under the **CirrusAudioFixup Non-Commercial License** ([LICENSE](LICENSE)).
- When adapting register sequences or driver logic from upstream Linux drivers (such as `cs35l41-hda`, `cs35l41-lib`, or `wm_adsp`), preserve upstream copyright notices and attribution.
