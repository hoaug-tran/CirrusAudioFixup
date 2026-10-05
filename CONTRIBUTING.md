# Contributing

Thanks for helping with CirrusAudioFixup.

This is kernel and amplifier code. Small, proven changes are better than broad rewrites.

## Ground rules

> [!IMPORTANT]
> Boot safety comes first. Do not broaden `Info.plist` matching unless you have IORegistry proof and a rollback plan.

> [!WARNING]
> Speaker safety matters. Do not add firmware, tuning, gain, boost, or power changes without hardware evidence.

## What a good change looks like

A good pull request is:

- small enough to review,
- scoped to one problem,
- backed by logs or tests,
- clear about hardware used,
- safe when firmware or playback fails.

Avoid:

- fake hardware support,
- placeholder firmware rows,
- broad ACPI matches,
- machine-specific names in generic code,
- cleanup mixed with behavior changes,
- large rewrites without test value.

## Required checks

Run these before opening a PR:

```bash
python Tests/reproduce_host.py
python Tests/check_registers.py
python Tests/check_transport.py
python Tests/check_diagnostics.py
python Tests/check_hda.py
python Tests/check_calibration.py
python Tests/check_bringup.py
python Tests/check_runtime.py
```

On macOS, also run:

```bash
xcodebuild -project CirrusAudioFixup.xcodeproj -target CirrusAudioFixup -configuration Release -sdk macosx CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO build
```

`ERROR` lines in host tests can be expected. These tests inject faults. The command must exit with code `0`.

## Hardware support checklist

For new laptop support, provide:

- laptop model,
- CPU platform,
- HDA codec id and subsystem id,
- ACPI device name under VoodooI2C,
- amplifier model,
- I2C addresses,
- amplifier device id and revision id,
- AppleALC layout id,
- Linux codec dump if available,
- IORegistry output with relevant `Cirrus_*` keys,
- source of firmware and tuning files if adding resources.

Do not map a new laptop to an existing tuning profile unless the hardware and acoustics are known to match.

## Firmware rules

Firmware resources are scoped per amplifier family.

For CS35L41, use:

```text
CirrusAudioFixup/Devices/CS35L41/Resources/Firmware.hpp
```

Future amplifier families should use their own device folder and their own `Resources/Firmware.hpp`.

Rules:

- Use real `.wmfw` and `.bin` files only.
- Keep table entries tied to real SSID and speaker id data.
- Do not add dummy rows.
- Do not mix firmware from another amplifier family.
- Keep import changes reproducible with `Tools/import_firmware.py`.

## Code style

| Element | Style |
| --- | --- |
| Classes and structs | `PascalCase` |
| Methods and functions | `lowerCamelCase` |
| Local variables | `lowerCamelCase` |
| Private members | `m` + `PascalCase` |
| Constants | `kPascalCase` |
| Namespaces | lowercase |
| Macros | `CIRRUS_UPPER_SNAKE_CASE`, only when needed |

Use hardware names exactly when they are part of the hardware identity:

- `CS35L41`
- `HDA`
- `WMFW`
- `OTP`

Inside variable names, acronyms become one word:

```cpp
bootDsp
hdaStreamActive
i2cAddress
gpioConfigured
```

## C++ and kext constraints

- Use C++17.
- No exceptions.
- No RTTI.
- No STL containers in kernel code.
- No `new` or `delete` outside IOKit allocation helpers.
- No dynamic global initialization with side effects.
- Use `#pragma once` for internal headers.
- Keep includes at the top of files.
- Avoid `../` include paths.
- Keep `CirrusAudioFixup` as the IOKit orchestration layer, not a register dump.

## Registers and constants

- Register addresses are `constexpr uint32_t`.
- Keep register names close to upstream or datasheet names.
- Prefer `cirrus::support::genMask`, `bit`, and `arraySize` over generic macros.
- Renaming a register constant needs a test or clear proof that values stayed equal.

## Comments

Use comments only when they help preserve hardware knowledge:

- quirks,
- errata,
- timing rules,
- ordering rules,
- upstream references,
- non-obvious safety choices.

Do not add comments that repeat what names already say. Do not leave commented-out code.

## Pull request rules

Before marking a PR ready:

- Tests pass.
- Build passes on macOS or CI.
- Logs are attached for hardware changes.
- Boot risk is explained.
- Speaker safety risk is explained.
- Rollback path is clear.
- README or templates are updated if user-facing behavior changed.

## License and attribution

This project uses `GPL-2.0-only`.

Keep attribution when porting behavior from Linux drivers such as `cs35l41-hda` and `cs_dsp`.
