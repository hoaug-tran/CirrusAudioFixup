## Summary

- What changed:
- Why it is needed:
- Hardware tested:

## Type

- [ ] Bug fix
- [ ] Hardware support
- [ ] Firmware or tuning resource
- [ ] HDA or AppleALC integration
- [ ] VoodooI2C transport
- [ ] Documentation
- [ ] Tests only

## Safety

- [ ] `Info.plist` matching stays boot-safe.
- [ ] No broad hardware match was added without IORegistry proof.
- [ ] No fake firmware or tuning entry was added.
- [ ] New firmware resources are scoped to the correct amplifier family.
- [ ] Failure path leaves amplifiers in a safe state.

## Required tests

Paste commands and result.

```text
python Tests/reproduce_host.py
python Tests/check_registers.py
python Tests/check_transport.py
python Tests/check_diagnostics.py
python Tests/check_hda.py
python Tests/check_calibration.py
python Tests/check_bringup.py
python Tests/check_runtime.py
```

## Build check

Paste command and result.

```text
xcodebuild -project CirrusAudioFixup.xcodeproj -target CirrusAudioFixup -configuration Release -sdk macosx CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO build
```

## Hardware evidence

Required for hardware or playback changes:

- Laptop model:
- CPU platform:
- HDA codec id:
- HDA subsystem id:
- ACPI match name:
- Amplifier model:
- I2C addresses:
- AppleALC layout:
- Firmware profile:

## Logs

Paste relevant output:

```text
log show --last boot --style syslog --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
ioreg -lw0 | grep -E 'Cirrus_(Driver|Diag|Trace|Playback|HDA|DSP|PCI|SSID)'
```

## Risk notes

- Boot risk:
- Speaker safety risk:
- Rollback path:
