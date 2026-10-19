# CS35L41 and Intel LPSS SPI development

Branch: `feat/cs35l41-spi-support`. Board integrations: [ASUS issue #2](https://github.com/hoaug-tran/CirrusAudioFixup/issues/2), [HP issue #3](https://github.com/hoaug-tran/CirrusAudioFixup/issues/3).

The transport layer is independent of CPU and board. Native drivers currently
implement Intel LPSS SPT/BXT/CNL PCI controller families and AMD AMDI0061/
AMDI0062 ACPI command controllers. The Intel identity table contains all 86
SPI IDs in Linux v6.12 intel-lpss-pci.c, with family-specific clock and CS
behavior; 7AAA is one entry, not a special gate. This is not support for every
SPI controller ever used on Intel/AMD systems.

The I2C personality uses VoodooI2C through driver-formatted register maps.
ACPI serial-bus resources select SPI or I2C automatically; SPI resources resolve
controller sources and bind native controllers. Both buses use the shipping
registered amplifier runtime for bring-up, firmware, group playback and PM.
Unknown boards remain read-only; writable operation requires exact verified
board and firmware bindings.
Driver-owned ACPI discovery resolves endpoints, channels and internal boost independently of OEM and controller path, with exact resource matching. The 103C89C6 dump is the present integration evidence, not a universal HP profile. Native macOS load and physical SPI playback are untested. See [HP evidence and target gates](hp_spi_bringup.md). No writable ASUS profile is supplied.
A CPU vendor does not identify a controller or supply board tuning.

| Driver | Source support | Limits / remaining work |
| --- | --- | --- |
| Intel LPSS SPT | PCI/MMIO owner + PIO | One native CS, 120 MHz root; hardware untested |
| Intel LPSS BXT | PCI/MMIO owner + PIO | Capabilities/native CS, 100 MHz root; hardware untested |
| Intel LPSS CNL/TGL | PCI/MMIO owner + PIO | Capabilities/native CS, 120/100 MHz root; hardware untested |
| AMD AMDI0061/0062 | ACPI/MMIO owner + command FIFO | Mode 0, 64-byte frames, 0.8–100 MHz; hardware untested |
| Intel LPT/BYT/BSW legacy, AMD PCI HID2 | Not implemented | Different private offsets or LPC resource mapping; need distinct profiles/drivers |
| Intel INTC1056/INTC1085 GPIO CS | Implemented for existing GPIO outputs | Evaluated ACPI mapping, exclusive provider, hardware ownership/TX lock and readback guards; target untested |
| Other controllers / GPIO families | Not implemented | Need documented register driver and ownership/power behavior |
| ACPI SPI IOService discovery | Implemented, automatic | Selected from `_CRS`; `-cirrusro` remains an optional forced read-only diagnostic mode; hardware validation ongoing |
| SPI amplifier lifecycle/playback | Connected to shipping driver runtime | Experimental HP 103C89C6 configuration included; native macOS and physical validation pending; no ASUS profile |

## Research and base selection

Intel 7AAA is listed by Linux v6.12 `intel-lpss-pci.c` under `tgl_spi_info`:
100 MHz root clock, LPSS_CNL_SSP controller register layout. The bus implementation
is `spi-pxa2xx.c`; LPSS glue supplies function reset, private clock and resource
handling. Intel Serial IO SPI is distinct from the flash/PCH SPI controller.
The generic endpoint `clockHz` is the peripheral maximum, not the root clock.
The LPSS fractional divider can reduce the actual SSP input below its profile root clock.

No verifiable open-source VoodooSPI controller/API for this device was found
in this investigation. Code does not assume that VoodooI2C exposes SPI or call
Apple private SPI classes. The new native base is implemented directly against
the controller register model; Linux supplies reference facts and quirks, not
an IOKit-compatible driver. Linux driver implementation code is not included.

Pinned references:

- [Intel PCI identity and root clock](https://github.com/torvalds/linux/blob/v6.12/drivers/mfd/intel-lpss-pci.c)
- [LPSS reset and private clock](https://github.com/torvalds/linux/blob/v6.12/drivers/mfd/intel-lpss.c)
- [LPSS SPI register behavior and chip-select quirks](https://github.com/torvalds/linux/blob/v6.12/drivers/spi/spi-pxa2xx.c)
- [SSP register definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/pxa2xx_ssp.h)
- [CS35L41 SPI framing](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41-lib.c#L738-L752)
- [SPI regmap read flag](https://github.com/torvalds/linux/blob/v6.12/drivers/base/regmap/regmap-spi.c)

## Controller implementation

`Transport/Intel/IntelLPSSSPIController.{hpp,cpp}` implements `SPIController`:

- Vendor/device profile lookup, retain/open PCI ownership and BAR0 mapping.
- Reject already opened controllers, missing/short mappings and non-D0 PCI.
- Preserve PCI memory-decode state and release mappings/ownership on failures.
- Serialize transfer and suspend/resume calls with an IOLock.
- No DMA, bus mastering, interrupt registration or undocumented Apple ABI.

`Transport/Intel/LPSSSPIEngine.hpp` implements the controller register engine:

- Validate SPI capabilities and enabled native chip selects on BXT/CNL; SPT exposes one native CS.
- Modes 0–3, eight-bit words, MSB first and active-low native CS.
- Read the fractional LPSS divider, calculate SCR without exceeding requested
  speed and reject unrepresentable rates.
- Preserve DMA reset; deassert function reset, enable the clock and run PIO.
- Hold one CS assertion for a complete full-duplex frame. Select CS while
  inactive, wait at least two input clock cycles and apply BXT/CNL clock gating quirks only for those profiles.
- Drain RX on writes too; at most eight bytes outstanding to bound FIFO use.
- Bound transfer time, drain stale RX, detect overruns/bit errors/inaccessible
  MMIO, stop clocks and release CS on accessible-hardware errors.
- Suspend blocks transfers; resume checks capabilities. Teardown leaves CS
  inactive and SSP disabled. No MMIO is attempted when PCI is already in D3.

Supported generic transfers are at most 4096 bytes and require estimated wire
time <=200 ms; actual transfer budget is twice that plus 10 ms. CS35L41 uses
smaller chunks. When a device disappears and reads return all ones, software
cannot guarantee the physical CS state; it marks the engine offline and stops
MMIO rather than attempting recovery writes against an inaccessible function.

Caller requirements: an owning IOService must arrange PCI D0, call suspend
before parent D3, resume after D0, and stop/join all clients before shutdown.
The controller is a reusable base, not an independently matched IOService.
It does not parse ACPI SpiSerialBus resources, support arbitrary GPIO controllers,
active-high CS, DMA/IRQ acceleration or arbitrary Intel SPI generations.
An IOLock serializes frames; the enclosing driver must also serialize complete
register read-modify-write operations. Bind/shutdown cannot race client calls.

## AMD command driver

`Transport/AMD/AMDSPIEngine.hpp` supports AMD AMDI0061 (V1) and AMDI0062 (V2)
register layouts. `AMDSPIController.{hpp,cpp}` supplies exclusive ACPI provider
ownership, MMIO mapping, serialized commands, D0 checks when `_PSC` exists,
and suspend/resume/cleanup. The enclosing service owns AML power sequencing.
It is not an automatically matched driver personality.

AMD uses a command byte/opcode followed by FIFO TX and then RX under a single
hardware command. `SPIController::command()` represents this separately from
simultaneous full-duplex `exchange()`. Intel uses the default bounded bridge;
AMD implements command directly and rejects full-duplex exchange. CS35L41
reads send the six-byte address/padding header before receiving payload, so
both drivers use the same register/firmware logic. The AMD frame limit reduces
CS35L41 firmware chunks automatically to 56 payload bytes.

AMD supports only mode 0, four native chip selections, at most 64 wire bytes,
and documented rates rounded down to the endpoint maximum (minimum 800 kHz).
V1/V2 opcode and trigger registers differ. The engine waits for completion on
writes too; a two-second timeout marks it offline without changing chip select
on a possibly running command. Recovery requires owner-managed hardware power/
reset. Generic CS polarity/GPIO switching and arbitrary AMD PCI controllers
are not implied by these ACPI drivers.

Reference: [AMD SPI registers and command semantics](https://github.com/torvalds/linux/blob/v6.12/drivers/spi/spi-amd.c).

## ACPI endpoint decoding

`Transport/ACPISPIResources.hpp` decodes the raw `_CRS` ResourceTemplate into
owned controller source paths, source indexes, native chip selects, maximum
clock and CPOL/CPHA modes. It validates descriptor bounds, EndTag/checksum,
SPI revisions and source strings. Four-wire, eight-bit, active-low consumer
connections are supported; slave mode, vendor framing, dependent alternatives
and unsupported formats are rejected. Failure leaves output unchanged.
This parser does not execute AML, resolve paths to PCI providers, authorize
exclusive bus ownership or infer missing reset/power resources.

Layout references: [ACPICA AML descriptors](https://github.com/acpica/acpica/blob/master/source/include/amlresrc.h)
and [serial resource conversion](https://github.com/acpica/acpica/blob/master/source/components/resources/rsserial.c).

## CS35L41 register implementation

`Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp` configures the shared `Core/RegisterMap.hpp` engine behind `core::RegisterIO` (see [extensible driver foundation](extensible_driver.md)):

- Scalar read/write, updateBits, bulk read/write and polling.
- Six-byte header: 32-bit BE register address plus 16 zero padding bits.
- Read flag 0x80 in the first address byte; dummy TX clocks provide read data.
- Four-byte-aligned address/payload validation and full-range overflow checks.
- Chunking to controller limits and a 256-byte payload cap.
- Read-only by default; writes require explicit board authorization.
- Failure propagation and unchanged scalar output on failed reads.

A bulk error may leave preceding chunks transferred; retries/rollback are not
implied. Polling bounds attempts, with controller transfer time additional.
Other DSP packing must be converted to 32-bit register-stride buffers by the
uploader before invoking the adapter, consistent with the current upload plan.

An owning service can bind the native controller and pass a checked ACPI
endpoint into `CS35L41SPIRegisterIO`. For example, with `controller` stored for
the service lifetime, `pci` already in D0 and `endpoint` parsed from real ACPI:

```cpp
IOReturn result = controller.bind(pci, this);
if (result != kIOReturnSuccess)
    return false;
cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO io(&controller, endpoint);
uint32_t deviceId = 0;
if (!io.read(0, &deviceId)) {
    controller.shutdown();
    return false;
}
```

The default adapter cannot write amp registers. Controller setup still writes
SPI controller registers to clock the read transaction; it is not a completely
write-free MMIO probe. CS/clock/mode are not inferred from an I2C address.

## Reusing Cirrus logic

The existing CS35L41 silicon class and firmware uploader consume `RegisterIO`.
Identity reads, applicable silicon trim/errata, DSP memory transfers, mailbox
and playback sequences can be reused behind a SPI adapter when validated for
the exact silicon and board. The byte framing changes inside the adapter.

HP and Lenovo use different power topologies. The HP 103C89C6 board configuration now supplies internal boost (1 uH, 4500 mA, 24 uF), Right/Left ordering and captured firmware resources. Shared transport/runtime code does not contain an HP model branch. The profile uses exact amplifier ACPI `_SUB` identity rather than translating a conflicting HDA controller/codec SSID. See [HP bring-up](hp_spi_bringup.md) for provenance and limits.

## Remaining integration and hardware checks

1. Build the modified tree on macOS and validate the loaded bundle/provider identities.
2. Confirm evaluated `cs-gpios`, GPIO ownership/D0, CS0 native and CS1 GPIO, and both read-only chip IDs.
3. Confirm firmware-released shared reset and platform/boost readback while muted; no per-AMP shared-reset pulse is implemented.
4. Validate matched channel firmware, calibration status and codec -> ASP -> amplifier route/clocks.
5. Exercise physical Right/Left, headphone switching, cold boot, quiet idle and sleep/wake before advertising machine support.

## Verification

Maintained regressions:

- `check_amd_spi.py`: production AMD V1/V2 command/FIFO engine, CS35L41
  read/write/RMW and firmware chunks, rates, failure and power guards.
- `check_amd_spi_lifecycle.py`: production AMD ACPI owner with allocation,
  ownership, mapping, power and rebind fault injection.
- `check_acpi_spi.py`: bounded ACPI parsing, truncation, revisions, checksum,
  multi-endpoint and malformed-buffer sanitizer checks.
- `check_spi_transport.py`: production CS35L41 framing, limits and errors.
- `check_intel_spi.py`: production PIO register engine and complete CS35L41
  read/write/firmware-chunk path through a FIFO model. Existing silicon probe,
  PLL, ASP and GPIO methods execute through the SPI driver in the model.
- `check_intel_spi_lifecycle.py`: production PCI owner against IOKit mocks,
  including ownership, D0/BAR failures, suspend, release and repeat bind.

Each SPI suite runs normally and with ASan/UBSan. They are included in
`Tests/run.py`. Debug and Release Xcode builds compile the native controller
sources into the kext. These checks do not validate electrical signals, real
Intel FIFO timing, physical PCI sleep transitions, firmware tuning or acoustic
output. No HP load/playback result has been obtained yet.

## Native SPI runtime capture

Add `-cirrusdbg` to inspect automatic ACPI SPI attachment for the `CSC3551` personality. Add `-cirrusro` only when intentionally suppressing reset and amplifier writes.
`NativeSPIHost` reads `_CRS`, resolves absolute/relative/parent AML paths against
IOACPIPlane, associates Intel PCI devices by their `acpi-device`, and binds the
existing Intel/AMD native owners. It supports up to 16 resource endpoints and
16 unique controllers, reuses shared controllers, rejects duplicate controller
plus chip-select bindings, and rolls back all owned resources after partial failure.

Register format is supplied by the silicon caller. The host uses generic
`SPIRegisterBus` and `RegisterMap`, so adding a new amplifier does not require
changing the controller owner. Discovery begins with read-only maps. Verified
board binding selects each driver format and authorizes writes unless
`-cirrusro` is set; no boot argument bypasses board verification. Speaker channels
come from verified endpoint bindings, never ACPI resource order. Published properties
include bind return, canonical controller path, chip-select, clock, mode and count.

Service suspend stops clients before suspending controllers; resume checks every
controller and suspends the group again on any failure. It does not force AML D0
or claim to coordinate every external controller power parent. Native AMD attachment
requires a working `_PSC` reporting D0; absence is rejected by this runtime owner
rather than inheriting the lower-level driver's owner-supplied D0 assumption. Stop removes event
sources before releasing register maps and controllers. PCI exclusivity and D0
checks remain mandatory. No native driver match is made on CPU vendor alone.

`check_native_spi.py` executes the production host with IOKit/controller mocks,
including relative paths, multi-controller ownership, duplicate aliases, missing
resources, partial rollback and PM failures, normally and with ASan/UBSan.
`check_transport.py` also exercises the production IOService register dispatch,
verifying that native accesses bypass I2C frames, preserve errors and honor sleep/stop.


## Registered SPI amplifier executor

`SPIAmplifierManager` connects native Intel/AMD controller ownership to
`AmplifierDriverRegistry` and `AmplifierManager`. Up to 16 resolved ACPI endpoints may use
different silicon register formats. A verified manifest plus actual provider HID,
exact endpoint/mode/clock matching and firmware identity precede binding; driver
identity and authorization precede initialization. Bind does not write amplifier
registers. The executor supports firmware, grouped playback, shutdown-before-
controller-suspend, controller-resume-before-bring-up and explicit confirmed
power-loss recovery. Failure evidence survives driver teardown.

The shipping IOService now binds its production CS35L41 driver through the
owned `AmplifierManager` using native SPI ports. Its existing firmware parser,
calibration and power sequences execute through those ports. The standalone
executor remains available for other owners. Tests exercise actual service
binding on both buses, registry dispatch, reversed channel ordering, read-only
maps, group commit/cancellation, PM and failure cleanup. HP assets come from the supplied Linux capture; no ASUS tuning
profile is supplied. Controller families and GPIO CS outside the implementation
table remain unsupported; host tests do not establish physical playback.


## Intel ACPI/PCI startup audit (2026-10-14)

The HP capture identifies CSC3551 under an ACPI SPI controller with PCI identity
8086:7AAA. Its captured `_CRS` decodes two SPI endpoints (CS0/CS1, 4 MHz, mode 0).
This resource fixture is maintained in `check_acpi_spi`; it does not authorize a
board profile or GPIO writes. Upstream Linux associates 7AAA with the 100 MHz
LPSS CNL-family contract, matching the existing controller profile.

The macOS capture available at that audit proved service creation and ACPI decoding, but stopped
at PCI association. It does not prove why the PCI query failed at that instant.
The default controller composition now uses PCI/ACPI identity contracts instead
of CPU brand. For a controller without a supported ACPI-only contract, the host
can wait at most five seconds for its exact ACPI path to be published as a PCI
service. It rechecks identity and uniqueness afterward. Custom factories are not
subjected to this default wait. Publication, timeout, duplicate arrival, wrong
class, ownership balance and CPU-independent selection are regression-covered.

Debug snapshots retain failed-service state before teardown, initial/current PCI
query counts, lookup result, raw `_CRS`, bounded `_DSD` with normalized ACPI
reference paths, and Intel bind stage/PMCSR/BAR/capability/status evidence. Quiet
Release omits Bootstrap audit snapshots; functional chip-select binding evaluates `_DSD` in both builds. The wait is functional startup policy, not a
debug-only behavior.

The 2026-10-17 Ubuntu capture supplies the previously missing `_DSD` and GPIO runtime evidence: native CS0, GPIO CS1 and shared reset. The new implementation/profile and remaining macOS gates are documented in [HP bring-up](hp_spi_bringup.md). DeviceSelection alone still does not identify native-CS wiring.

References:
- https://developer.apple.com/documentation/kernel/ioservice/1811164-waitformatchingservice
- https://github.com/torvalds/linux/blob/master/drivers/mfd/intel-lpss-pci.c
- https://code.googlesource.com/linux/torvalds/linux/+/78273df7f646f8daf2604ec714bea0897cd03aae/sound/pci/hda/cs35l41_hda_property.c

See [the configuration and binary evidence audit](platform_evidence_audit.md) for the separation of driver reuse, firmware properties, resource identity and unimplemented topologies.
