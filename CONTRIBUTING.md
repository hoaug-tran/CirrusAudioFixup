# Contributing to CirrusAudioFixup

Thank you for contributing to CirrusAudioFixup. As a kernel-level audio extension dealing directly with hardware power rails and amplifier DSPs, reliability and speaker safety are our top priorities.

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
   - Source of firmware (`.wmfw`) and calibration binaries (`.bin`) from upstream `linux-firmware`.

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

Before submitting a pull request, ensure all host-side tests and the release build pass cleanly:

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
```
> [!NOTE]
> Tests include intentional fault-injection sweeps. Intermittent `ERROR` log messages in the test output are expected; the suite must finish with exit code `0` and print `PASS`.

### 2. Build on macOS
```bash
xcodebuild -project CirrusAudioFixup.xcodeproj \
           -target CirrusAudioFixup \
           -configuration Release \
           -sdk macosx \
           CODE_SIGNING_REQUIRED=NO \
           CODE_SIGN_IDENTITY="" \
           CODE_SIGNING_ALLOWED=NO build
```

---

## License & Attribution

- This project is licensed under the **CirrusAudioFixup Non-Commercial License** ([LICENSE](LICENSE)).
- When adapting register sequences or driver logic from upstream Linux drivers (such as `cs35l41-hda`, `cs35l41-lib`, or `wm_adsp`), preserve upstream copyright notices and attribution.
