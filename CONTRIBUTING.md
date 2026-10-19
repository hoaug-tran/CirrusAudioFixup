# Contributing to CirrusAudioFixup

Changes to this driver can affect amplifier power rails and speaker output. Keep a recoverable EFI and distinguish source checks from physical playback validation.

---

## Before changing the driver

> [!IMPORTANT]
> **Hardware checks:**
> - Never submit unverified firmware, aggressive gain boosts, or experimental voltage changes without real hardware validation.
> - Do not broaden `Info.plist` matches or add placeholder entries without verified IORegistry dumps and an emergency rollback plan.
> - Keep pull requests focused on a single feature, bugfix, or hardware target.

---

## Issues and pull requests

Use the issue forms for reproducible bugs, new hardware or concrete improvements. Include the exact build and evidence from the affected machine. Do not report only latest, assume a bus from an OEM name, or present an untested result as passed.

Normal change PRs target `develop`; identify release PRs separately. Use the PR template to explain the problem, result, source references and checks. Documentation-only changes do not need a new hardware run. Driver and board changes must separate host/build evidence from actual loaded-bundle and speaker tests.

## Adding support for new hardware

Use the Hardware support form and include:

- Exact machine, CPU and OS versions actually tested.
- Codec/HDA subsystem identity and amplifier model/revision, if known.
- I2C or SPI evidence, ACPI HID/_SUB, controller and endpoints. Do not assume every amplifier is a VoodooI2C device.
- A Linux or Windows report from the new general collectors, plus macOS IORegistry if available. Include collection errors and partial output where necessary.
- AppleALC speaker-route evidence from the codec dump and matching resources; mark an unverified route explicitly.
- Matching firmware/tuning source and hashes, known speaker/channel/reset/boost data, and whether measured calibration is available. Missing values must stay unknown.

Read [collector options](docs/audio_collectors.md) and [codec route checks](docs/codec_route.md). Do not force another board's SSID, substitute channel tuning or disable DSP protection merely to complete a report.

---

## Code conventions

### C++ and the kernel

- **Standard**: C++17.
- **Constraints**:
  - No C++ exceptions (`-fno-exceptions`).
  - No RTTI (`-fno-rtti`).
  - No STL containers in kernel code (`std::vector`, `std::map`, etc. are forbidden in kernel space; use IOKit collections or fixed-size structures).
  - Use `#pragma once` for all internal header files.
  - No raw `new` / `delete` outside of IOKit memory allocation helpers (`IOLockAlloc`, `IOMalloc`, `IOFree`).

### Style and naming

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
| **Constants** | `k` + `PascalCase` | `kMaxAmplifiers`, `kRegPowerControl1` |
| **Enum types / values** | `PascalCase` | `AmplifierModel::CS35L41`, `AmplifierStage::Ready` |
| **Namespaces** | `lowercase` | `cirrus::devices::cs35l41` |
| **Macros** | `UPPER_SNAKE_CASE` | `CIRRUS_LOG`, `CIRRUS_ERR` |

### Source layout and naming

Use PascalCase `.hpp` / `.cpp` filenames describing the main type or narrowly
scoped responsibility. Keep acronym spelling consistent: `I2C`, `SPI`, `ACPI`,
`HDA`, `WMFW`, `CS35L41`. Public record fields and local variables use
lowerCamelCase; private class state uses `mPascalCase`. Name functions for their
operation: `bringUpAmplifiers`, `findAmplifierById`, `initializeCS35L41Device`.
Avoid vague implementation labels such as `Real`, `FixupAdapter`, or an empty
controller source. Related helper types may share one header; one file per tiny
struct is not required.

| Directory | Responsibility | Examples |
| --- | --- | --- |
| `Core/` | Bus-independent register and amplifier contracts | `RegisterIO`, `RegisterMap`, `BusEndpoint`, `AmplifierLifecycle`, `AmplifierDescriptors` |
| `Transport/` | Bus/provider transactions and controller ownership | `I2CController`, `I2CRegisterBus`, `VoodooI2CProvider`, `NativeSPIHost` |
| `Transport/Intel/`, `Transport/AMD/` | Documented controller families | `IntelLPSSSPIController`, `AMDSPIController` |
| `Platform/` | Board identity, resources, driver selection and owned runtime | `BoardConfiguration`, `AmplifierDriverRegistry`, `AmplifierManager` |
| `Devices/Amplifiers/<Model>/` | Silicon registers, framing, errata, lifecycle and resources | `CS35L41DriverFactory`, `CS35L41SPIRegisterIO` |
| `Firmware/<Format>/` | Format-specific parser, mapping and upload | `WMFWParser`, `WMFWMemoryMapper`, `WMFWUploader` |
| `Diagnostics/`, `Support/` | Transfer evidence and small kernel utilities | `DiagnosticTracer`, `Logging` |

Generic extension points must not acquire one AMP's model name or register
layout. A protocol-specific firmware parser should retain its protocol name.
Adding another AMP does not mean treating WMFW, CS35L41 framing or Intel LPSS
registers as universal. `AmplifierModel` describes speaker AMP silicon, distinct
from the Realtek HDA codec. `AmplifierDevice` retains register-level helpers;
shipping lifecycle ownership lives in `AmplifierManager` and driver hooks.

### Driver boundaries

Keep silicon registers, firmware and lifecycle in the amplifier module. Keep controller ownership, framing and PM in transport modules. The service supplies discovery, verified board bindings and serialized runtime coordination. Read the [framework guide](docs/extensible_driver.md) before adding a module.

Factories and destructors perform no hardware I/O. Shutdown completes before drivers and their borrowed register ports are released. Preserve the original failure when cleanup also fails.

Keep published boot arguments, IORegistry properties and provider ABI stable unless the change includes a migration. IOKit entry classes retain the names used by their metaclasses and personalities.

### Documentation and comments

- Put comments on their own lines above the relevant code. Explain ownership, hardware ordering, units, errata or failure behavior when the code alone is unclear. Keep source and revision references next to hardware constants.
- Do not leave commented-out code, fix history, progress notes or comments that repeat the statement. Use the same plain wording in documentation; keep changelog entries in CHANGELOG.md.

---

## Validation

Run from the repository root. Host tests need Python 3, Bash and a C++17 compiler named `g++`. Validate both Debug and Release; their logging defaults differ, but hardware checks must remain the same.

### 1. Run the maintained host suite

```bash
python3 Tests/run.py
```

Use `python3 Tests/run.py --audit` when the optional Windows package-audit checks are relevant. The runner lists the maintained suites; do not replace it with an old hand-picked subset.

> [!NOTE]
> Tests include intentional fault-injection sweeps. Intermittent `ERROR` log messages in the test output are expected; the suite must finish with exit code `0` and print `PASS`.

### 2. Build on macOS

Prepare the pinned MacKernelSDK and Lilu checkouts using the [build instructions](README.md#build-from-source). Do not replace modified dependency checkouts without review.

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

## License and attribution

- This project is licensed under the **CirrusAudioFixup Non-Commercial License** ([LICENSE](LICENSE)).
- When adapting register sequences or driver logic from upstream Linux drivers (such as `cs35l41-hda`, `cs35l41-lib`, or `wm_adsp`), preserve upstream copyright notices and attribution.

## Project layout

| Path | Contents |
| --- | --- |
| `CirrusAudioFixup/Core` | Shared register, endpoint and lifecycle contracts |
| `CirrusAudioFixup/Devices/Amplifiers` | Per-family amplifier drivers and resources |
| `CirrusAudioFixup/Platform` | Board discovery, driver registry and HDA integration |
| `CirrusAudioFixup/Transport` | I2C, native SPI, ACPI and controller access |
| `Tests` | Maintained host regressions and fixtures |
| `Tools` | Build, collection, import and release tools |
| `docs` | Hardware guides and technical references |
