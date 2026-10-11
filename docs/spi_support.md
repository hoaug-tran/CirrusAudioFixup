# CS35L41 SPI development

Branch: `codex/cs35l41-spi-support`. Tracking: [HP hardware issue #3](https://github.com/hoaug-tran/CirrusAudioFixup/issues/3).

This branch adds a CS35L41 SPI **register adapter**, not a working Intel SPI
controller driver or an enabled HP personality. The shipping IOService path
still dispatches through VoodooI2C. Installing this branch alone does not make
HP internal speakers work. No CSC3551 match or HP reset/boost override is added.

## Implemented

`Transport/SPIController.hpp` defines a synchronous controller contract:
explicit chip-select, clock Hz and SPI mode, full-duplex 8-bit MSB-first frames,
a reported maximum transfer size, and IOReturn failures. It is a project C++
interface, not an Apple/VoodooSPI platform-function ABI. The owning driver must
serialize the bus, handle power/stop state, bound waits and deassert CS on errors.
The caller owns the controller lifetime and serialization of register RMW.

`Transport/CS35L41SPITransport.hpp` implements `core::RegisterIO`:

- Scalar read/write and read-modify-write.
- Raw bulk read/write, chunked to the controller limit and a 256-byte payload cap.
- Four-byte-aligned address/payload checks and full-range overflow validation.
- Six-byte header: big-endian 32-bit register and 16 zero padding bits.
- Read flag 0x80 in the first register byte, dummy transmit clocks for reads,
  and big-endian scalar values. Each complete frame uses one CS assertion.
- Read-only by default; writes require explicit caller authorization.
- Failure propagation, unchanged scalar output on read failure and bounded poll
  attempts. Poll duration includes additional controller transfer time.

A bulk error can leave earlier chunks transferred; no automatic retries or
rollback are implied. This adapter accepts 32-bit register-stride payloads;
other DSP packing must be converted by the uploader before invoking it.

Protocol references (Linux v6.12):

- [Cirrus CS35L41 register-map configuration](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41-lib.c#L738-L752)
- [SPI regmap bus read flag and framing](https://github.com/torvalds/linux/blob/v6.12/drivers/base/regmap/regmap-spi.c)

## Required next work

1. Collect original DSDT/all SSDTs, Windows driver/tuning exports and macOS
   IORegistry. Reconcile Windows SUBSYS 103C89C6 with dump SUBSYS 103C8CA4.
2. Determine who owns Intel Serial IO SPI 7AAA on the target macOS. Confirm
   controller BAR/resources, clock, CS wiring/polarity, reset and power handling
   from actual evidence. A PCI device ID alone is not a controller API.
3. Implement `SPIController` against a verified controller driver, or develop
   the missing controller driver. No BAR offsets, GPIOs or clocks are guessed
   in this branch. Controller access must coordinate exclusive ownership.
4. Add service endpoint discovery and transport selection. Current amp state
   and diagnostic labels use I2C addresses; migrate those to explicit bus and
   endpoint identity before attaching SPI hardware. Preserve power/stop gates.
5. Validate read-only chip ID/revision first, then authorize a board profile
   and reset/boost configuration with matching firmware/tuning evidence.
6. Exercise full codec → ASP → amplifier playback, both channels, jack switching,
   cold boot and sleep/wake before advertising HP support.

Upstream 103C89C6 specifies internal boost (1 uH, 4500 mA, 24 uF), two amps and
Right/Left order; it corrects a BIOS _DSD boost declaration. It is not permission
to apply those parameters to the conflicting 103C8CA4 identity.

## Validation

`python3 Tests/check_spi_transport.py` executes the production header with a
mock SPI controller and checks read/write wire bytes, receive offsets, bulk
boundaries, controller limits, error stops, read-only policy, invalid ranges,
RMW and polling. It runs normally and with ASan/UBSan. It does not validate
physical SPI signals, Intel controller registers, HP firmware or speaker output.

The suite is included in `Tests/run.py`; Debug and Release Xcode builds compile
the headers alongside existing I2C code. Header compilation is not runtime
attachment. No load/playback result has been obtained on the HP yet.
