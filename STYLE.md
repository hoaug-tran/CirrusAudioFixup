# CirrusAudioFixup C++ / Kext Coding Standard

Modern C++17 + IOKit conventions + kernel constraints + hardware-driver naming.
This document is normative. Code that violates it must not be merged.

## 1. Language and build constraints

- C++17, exceptions OFF, RTTI OFF, no STL containers, no `new`/`delete` outside IOKit allocation helpers.
- No dynamic initialization of globals with side effects.
- No `#include` anywhere except the top of a file.

## 2. Naming

| Element | Convention | Example |
|---|---|---|
| Class / struct | PascalCase | `CS35L41Device` |
| Enum type | PascalCase, prefer `enum class` | `DriverStage` |
| Enum value | PascalCase | `DriverStage::Probe` |
| Function / method | lowerCamelCase | `bootDsp()` |
| Local / parameter | lowerCamelCase | `finalCrc` |
| Private member | `m` + PascalCase | `mDeviceId` |
| Boolean | `is` / `has` / `needs` / `supports` prefix | `isDspAlive` |
| Compile-time constant | `k` + PascalCase | `kPowerControl1` |
| Namespace | lowercase | `cirrus::devices::cs35l41` |
| Macro (only when unavoidable) | `CIRRUS_UPPER_SNAKE_CASE` | `CIRRUS_LOG` |
| File containing a class | identical to class name | `CS35L41Device.hpp` |
| Directory | PascalCase | `Firmware/WMFW/` |

### Acronyms

- Hardware / product / format identifiers stay exact as type names: `CS35L41`, `ALC287`, `WMFW`, `OTP`.
- Inside function, variable and member names, an acronym is one word: `bootDsp`, `hdaStreamActive`, `i2cAddress`, `gpioConfigured`.

### Accessors

No `get` prefix: `name()`, `address()`, `deviceId()`, `revisionId()`. Setters use `setX()`.

## 3. Types

- Project-internal code uses `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t`, `size_t`, `bool`.
- Apple types (`UInt32`, `SInt32`, `IOReturn`, `IOOptionBits`, ...) only at IOKit API boundaries.
- Hardware register values and addresses are always `uint32_t`.

## 4. Namespaces

- `CirrusAudioFixup` (the `IOService` subclass) stays in the global namespace because of `IOClass` / Info.plist.
- Everything else lives in:
  - `cirrus::core`
  - `cirrus::support`
  - `cirrus::diagnostics`
  - `cirrus::transport`
  - `cirrus::platform::hda`
  - `cirrus::firmware::wmfw`
  - `cirrus::devices::cs35l41`
- No `using namespace` in headers.

## 5. Constants, registers, macros

- Registers are `constexpr uint32_t` inside `cirrus::devices::<device>::registers`.
- The upstream datasheet / Linux name is recorded with a trailing comment on the same line:

```cpp
namespace cirrus::devices::cs35l41::registers {
constexpr uint32_t kPowerControl1 = 0x00002014; // CS35L41_PWR_CTRL1
}
```

- No generic macros (`GENMASK`, `ARRAY_SIZE`, `BIT`, `MIN`, `MAX`). Use `cirrus::support` constexpr / template utilities (`genMask`, `bit`, `arraySize`).
- A macro is allowed only for logging (`__func__` / file capture) or conditional compilation, and must be prefixed `CIRRUS_`.
- Bitmask enums use `enum class` plus the operators in `Support/BitUtils.hpp`.
- Renaming a register constant requires a host test that asserts the old and new values are equal before the old definition is removed.

## 6. Headers and includes

- Every internal header uses `#pragma once`.
- Includes are rooted at `CirrusAudioFixup/`: `#include "Core/AudioDevice.hpp"`. No `../`.
- Order (enforced by `.clang-format`), one blank line between groups:
  1. Own header (`"X.hpp"` for `X.cpp`, `"CirrusAudioFixup.hpp"`)
  2. Project subsystem headers (`"Core/..."`, `"Devices/..."`, ...)
  3. Other quoted headers
  4. Third-party kext headers (`<VoodooI2C/...>`, `<Headers/...>`)
  5. Kernel headers (`<IOKit/...>`, `<libkern/...>`, `<mach/...>`)
  6. Other system headers
- If a group genuinely depends on order, isolate it with `// clang-format off` / `// clang-format on`, with the reason on the line above.

## 7. Comments

Comments are allowed only for:

- hardware quirks and errata,
- ordering / timing constraints and invariants,
- upstream cross-references (datasheet section, Linux symbol name),
- explaining why a non-obvious choice was made.

Comments that restate what a name already says are forbidden:

```cpp
/* Returns the device name identifier */
virtual const char *getName() const = 0;
```

Good:

```cpp
// CS35L41 ignores protected register writes until OTP boot completes.
if (!waitForOtpBoot(io)) {
    return false;
}
```

No commented-out code. No banner or separator comments. No change-log comments.

## 8. Class layout

```cpp
class CS35L41Device final : public core::AudioDevice {
public:
    explicit CS35L41Device(uint8_t address);

    bool probe(core::RegisterIO& io) override;
    bool bootDsp(core::RegisterIO& io) override;

    const char* name() const override;
    uint8_t address() const;

private:
    bool waitForOtpBoot(core::RegisterIO& io);
    bool applyErrata(core::RegisterIO& io);

    uint8_t mAddress;
    uint32_t mDeviceId{0};
};
```

- Order: public API, protected (only if required), private behavior, private state.
- Leaf classes are `final`. Overrides use `override`, never repeated `virtual`.
- Single-argument constructors are `explicit`.
- `.cpp` definitions follow header declaration order.
- One primary class per file.

## 9. Layout

```
CirrusAudioFixup/
├── Core/            AudioDevice, RegisterIO, Types
├── Transport/       VoodooI2CTransport
├── Devices/CS35L41/ CS35L41Device, Hardware/{Registers,OTPMap,Errata}, Resources/
├── Firmware/WMFW/   WMFWFormat, WMFWParser, FirmwareMapper, FirmwareUploader
├── Diagnostics/     DiagnosticState, RegisterTrace, DriverVerdict
├── Platform/HDA/    HDAController
├── Support/         BitUtils, Logging
├── CirrusAudioFixup.hpp / .cpp   IOKit lifecycle and orchestration only
└── Info.plist
```

`CirrusAudioFixup` must not contain device-specific register knowledge.

## 10. Formatting and line endings

- `.clang-format` is authoritative. `.editorconfig` covers non-C++ files.
- LF everywhere, enforced by `.gitattributes`.

## 11. Acceptance criteria

A change is complete only when all of the following hold:

1. `clang-format --dry-run --Werror` passes on every touched source file.
2. `git diff --check` is clean.
3. Host regression tests in `Tests/` pass.
4. Static checks pass.
5. Xcode kext build succeeds on macOS.
6. Runtime load / probe succeeds.
7. Physical playback validation for changes touching device bring-up, firmware, or HDA.

Items 1–4 are mandatory in CI. Items 5–7 are mandatory before a release.
