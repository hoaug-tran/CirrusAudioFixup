# CS35L41 and Intel LPSS SPI development

Branch: `feat/cs35l41-spi-support`. Tracking: [HP hardware issue #3](https://github.com/hoaug-tran/CirrusAudioFixup/issues/3).

The branch implements a CS35L41 SPI register adapter and a native Intel LPSS
PIO controller base for PCI 8086:7AAA. The existing shipping IOService path
still dispatches through VoodooI2C: PCI/ACPI discovery, endpoint wiring, parent
power notifications and HP board authorization are not connected to this base
yet. Installing the branch alone does not make HP internal speakers work.
No automatic CSC3551/PCI match, guessed HP GPIO or firmware override is added.

## Research and base selection

Intel 7AAA is listed by Linux v6.12 `intel-lpss-pci.c` under `tgl_spi_info`:
100 MHz root clock, LPSS_CNL_SSP controller register layout. The bus implementation
is `spi-pxa2xx.c`; LPSS glue supplies function reset, private clock and resource
handling. Intel Serial IO SPI is distinct from the flash/PCH SPI controller.
The generic endpoint `clockHz` is the peripheral maximum, not the root clock.
The LPSS fractional divider can reduce the actual SSP input below 100 MHz.

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

- Exact vendor/device validation, retain/open PCI ownership and BAR0 mapping.
- Reject already opened controllers, missing/short mappings and non-D0 PCI.
- Preserve PCI memory-decode state and release mappings/ownership on failures.
- Serialize transfer and suspend/resume calls with an IOLock.
- No DMA, bus mastering, interrupt registration or undocumented Apple ABI.

`Transport/Intel/LPSSSPIEngine.hpp` implements the controller register engine:

- Validate SPI capabilities and enabled native chip selects.
- Modes 0–3, eight-bit words, MSB first and active-low native CS.
- Read the fractional LPSS divider, calculate SCR without exceeding requested
  speed and reject unrepresentable rates.
- Preserve DMA reset; deassert function reset, enable the clock and run PIO.
- Hold one CS assertion for a complete full-duplex frame. Select CS while
  inactive, wait at least two input clock cycles and handle CNL clock gating.
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
It does not parse ACPI SpiSerialBus resources, support external GPIO CS,
active-high CS, DMA/IRQ acceleration or arbitrary Intel SPI generations.
An IOLock serializes frames; the enclosing driver must also serialize complete
register read-modify-write operations. Bind/shutdown cannot race client calls.

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

`Transport/CS35L41SPITransport.hpp` implements `core::RegisterIO`:

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
endpoint into `CS35L41SPITransport`. For example, with `controller` stored for
the service lifetime, `pci` already in D0 and `endpoint` parsed from real ACPI:

```cpp
IOReturn result = controller.bind(pci, this);
if (result != kIOReturnSuccess)
    return false;
cirrus::transport::CS35L41SPITransport io(&controller, endpoint);
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

This does not establish identical HP and Lenovo boards. The current service
still owns I2C endpoint addresses, reference GPIO reset, ASP channel routing,
firmware selection and subsystem gating. Those need a verified HP profile and
transport discovery before enabling automatic full-driver flow. Upstream
103C89C6 uses internal boost (1 uH, 4500 mA, 24 uF), two amps and Right/Left
index order, correcting an ACPI boost-type error. The supplied codec dump
instead says 103C8CA4; that discrepancy remains unresolved.

## Remaining integration and hardware checks

1. Collect original DSDT/all SSDTs, Windows driver/tuning exports and macOS
   IORegistry. Reconcile 103C89C6 versus 103C8CA4.
2. Resolve controller ownership and ACPI native CS/clock/mode and power resources;
   confirm whether an additional GPIO CS is required. Do not steal a controller
   used by another SPI peripheral.
3. Add the actual IOService discovery/parent-PM path and transport selection;
   migrate endpoint/diagnostic identity away from I2C-only addresses.
4. Validate read-only chip ID/revision on HP, then reset/boost/firmware/tuning.
5. Exercise codec → ASP → amplifier playback, both channels, jack switching,
   cold boot, quiet idle and sleep/wake before advertising machine support.

## Verification

Maintained regressions:

- `check_acpi_spi.py`: bounded ACPI parsing, truncation, revisions, checksum,
  multi-endpoint and malformed-buffer sanitizer checks.
- `check_spi_transport.py`: production CS35L41 framing, limits and errors.
- `check_intel_spi.py`: production PIO register engine and complete CS35L41
  read/write/firmware-chunk path through a FIFO model. Existing silicon probe,
  PLL, ASP and GPIO methods execute through the SPI backend in the model.
- `check_intel_spi_lifecycle.py`: production PCI owner against IOKit mocks,
  including ownership, D0/BAR failures, suspend, release and repeat bind.

Each SPI suite runs normally and with ASan/UBSan. They are included in
`Tests/run.py`. Debug and Release Xcode builds compile the native controller
source into the kext. These checks do not validate electrical signals, real
Intel FIFO timing, physical PCI sleep transitions, firmware tuning or acoustic
output. No HP load/playback result has been obtained yet.
