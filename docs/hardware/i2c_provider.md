# Custom I2C Provider Interface

Cirrus smart amplifiers on I2C buses (such as Lenovo Legion and ASUS Zenbook laptops) require the custom [VoodooI2C `main` branch](https://github.com/hoaug-tran/VoodooI2C/tree/main).

The custom provider exposes the `VoodooI2CTransferToAddress` platform function, which allows CirrusAudioFixup to communicate with multiple amplifier addresses through a single controller nub.

> [!IMPORTANT]
> The upstream VoodooI2C `master` branch does not include this addressed-transfer interface. You must use the build from the `main` branch.

---

## Request Memory Layout

[`VoodooI2CProvider.hpp`](../../CirrusAudioFixup/Transport/I2C/VoodooI2CProvider.hpp) defines a 40-byte, 8-byte-aligned structure on `x86_64`:

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | 1 byte | `address` | 7-bit target I2C device address |
| `0x08` | 8 bytes | `writeBuffer` | Pointer to write data buffer |
| `0x10` | 2 bytes | `writeLength` | Number of bytes to write |
| `0x18` | 8 bytes | `readBuffer` | Pointer to read output buffer |
| `0x20` | 2 bytes | `readLength` | Number of bytes to read |

Buffers must remain valid until the synchronous call completes. Combined read/write operations submit the register address prefix and receive readback data in a single repeated-START transaction.

---

## Verifying Provider Status

To check if the custom provider is active in macOS:

```bash
# Check loaded VoodooI2C extensions
kmutil showloaded | grep -i voodooi2c

# Verify I2C provider attachment in IORegistry
ioreg -lw0 -p IOService -r -c VoodooI2CDeviceNub
```

If transfers fail, check `Cirrus_Communication_Status` in IORegistry. Ensure that:
* `VoodooI2C.kext` and `VoodooI2CHID.kext` are loaded.
* `CirrusAudioFixup.kext` is loaded **after** `VoodooI2C.kext` in OpenCore's `Kernel → Add`.

[Back to main guide](../../README.md#requirements)
