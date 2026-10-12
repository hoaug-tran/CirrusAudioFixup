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
| `Devices/<Model>/` | Silicon registers, framing, errata, lifecycle and resources | `CS35L41DriverFactory`, `CS35L41SPIRegisterIO` |
| `Firmware/<Format>/` | Format-specific parser, mapping and upload | `WMFWParser`, `WMFWMemoryMapper`, `WMFWUploader` |
| `Diagnostics/`, `Support/` | Transfer evidence and small kernel utilities | `DiagnosticTracer`, `Logging` |

Generic extension points must not acquire one AMP's model name or register
layout. A protocol-specific firmware parser should retain its protocol name.
Adding another AMP does not mean treating WMFW, CS35L41 framing or Intel LPSS
registers as universal. `AmplifierModel` describes speaker AMP silicon, distinct
from the Realtek HDA codec. `AmplifierDevice` retains register-level helpers;
shipping lifecycle ownership lives in `AmplifierManager` and driver hooks.

### Framework target

Adding a register-controlled AMP should require its `Devices/<Model>` driver,
registry definition and verified board/firmware data. It should not require a
silicon branch, new register sequence or descriptor allowlist in the service.
Each driver owns its implementation and state; `AmplifierHost` supplies common
board authorization, boot/resource policy, route observations and diagnostic
publication. The production CS35L41 driver owns its live state, firmware parsing,
silicon operations, protection and playback helpers. Its factory consumes only
`RegisterIO`, `AmplifierHost`, board bindings and immutable asset views. No
service-member callback or service cast is part of the AMP template.

AMP factories and destructors perform no hardware I/O. The serialized runtime calls
safe shutdown before destroying drivers, then releases host/port/map/controller
resources. A driver can report readiness and pending resources through
`AmplifierReport` and `prepareBringUpRetry`; pending polling must not reset hardware.
Resource completion re-enters group bring-up using the same owned driver objects.
Original diagnostics and cleanup failures must survive teardown/rebind. Traced
register views preserve operation sources and precious-register diagnostic policy.

A provider-backed communication adapter implements the existing controller
transaction contract and is selected by verified platform data. AMP code consumes
`RegisterIO`; it must not learn provider ABI, CPU vendor or bus framing. Factory
failure does not silently select another communication implementation.
Native SPI modules implement `NativeSPIControllerOwner` and return their owned
transaction port and PM methods through a `spiControllerFactory`. The host retains
ACPI targets, shares one owner per target and releases register maps/buses before
owners. Intel/AMD binding and D0 policy stay in their modules; new owners do not
require a controller branch in `NativeSPIHost`. Native
MMIO/DMA owners still need resource, arbitration and PM contracts; SoundWire needs
its separate stream/controller subsystem. Host fixture extensions are not shipping
hardware support.

IOKit entry classes keep their established globally scoped names because
`OSMetaClass`, personalities and loader symbols use those identities. Keep
boot arguments, IORegistry property keys and the custom VoodooI2C request ABI
stable during source renaming. Changes to those interfaces need their own
migration. Existing silicon helper implementations may still live in the service
translation unit; their names must expose that silicon scope.

These are this project's C++/IOKit conventions, not a universal kernel folder
standard. Apple [IOService source](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOService.h)
uses named C++ service interfaces; Linux's [coding style](https://docs.kernel.org/process/coding-style.html#naming)
requires descriptive global identifiers, while its [CS35L41 I2C driver](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/cs35l41-i2c.c)
keeps silicon/bus binding separate. Apply those responsibilities without importing
Linux C spelling rules into this C++ kext. Run `Tests/check_source_layout.py` and
the complete host/build checks after moving or renaming files.

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
python3 Tests/check_extensible_core.py
python3 Tests/check_cs35l41_driver.py
python3 Tests/check_amd_spi.py
python3 Tests/check_amd_spi_lifecycle.py
python3 Tests/check_acpi_spi.py
python3 Tests/check_spi_transport.py
python3 Tests/check_intel_spi.py
python3 Tests/check_intel_spi_lifecycle.py
python3 Tests/check_diagnostics.py
python3 Tests/check_hda.py
python3 Tests/check_calibration.py
python3 Tests/check_bringup.py
python3 Tests/check_runtime.py
python3 Tests/check_audio_events.py
python3 Tests/check_architecture.py
python3 Tests/check_release.py
python3 Tests/check_build_policy.py
python3 Tests/check_tools.py
python3 Tests/check_tuning.py
python3 Tests/check_windows_payloads.py
```
> [!NOTE]
> Tests include intentional fault-injection sweeps. Intermittent `ERROR` log messages in the test output are expected; the suite must finish with exit code `0` and print `PASS`.

### 2. Build on macOS

Prepare the pinned MacKernelSDK and Lilu checkouts using the [build instructions](README.md#building-and-testing). Do not replace modified dependency checkouts without review.

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
