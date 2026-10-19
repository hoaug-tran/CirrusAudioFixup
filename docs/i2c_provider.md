# I2C provider interface

I2C needs the custom [VoodooI2C main branch](https://github.com/hoaug-tran/VoodooI2C/tree/main). Its `VoodooI2CTransferToAddress` platform function lets CirrusAudioFixup access each amplifier address through the same provider. The fork's `master` branch does not provide this interface. A version number alone does not identify a compatible build.

## Request layout

`Transport/VoodooI2CProvider.hpp` defines a 40-byte, 8-byte-aligned request on x86_64. Do not pack or reorder it.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 byte | Seven-bit device address |
| 8 | 8 bytes | Write-buffer pointer |
| 16 | 2 bytes | Write length |
| 24 | 8 bytes | Read-buffer pointer |
| 32 | 2 bytes | Read length |

Buffers remain valid until the synchronous call returns. A combined read submits the write prefix and read buffer to the same address in one controller request. Keep the native return before another operation replaces it. The controller owns repeated-START timing, arbitration and completion.

## Source and checks

The interface is defined in the [nub header](https://github.com/hoaug-tran/VoodooI2C/blob/1a1ae9a26a74ec6b7e32a5d07f6a6ae9413bf298/VoodooI2C/VoodooI2C/VoodooI2CDevice/VoodooI2CDeviceNub.hpp#L30) and [handler](https://github.com/hoaug-tran/VoodooI2C/blob/1a1ae9a26a74ec6b7e32a5d07f6a6ae9413bf298/VoodooI2C/VoodooI2C/VoodooI2CDevice/VoodooI2CDeviceNub.cpp#L423) at commit `1a1ae9a26a74ec6b7e32a5d07f6a6ae9413bf298`. The provider has its own GPL-3.0 license.

The adapter has compile-time layout assertions. `Tests/check_transport.py` checks addressed requests, register framing and error handling. Physical bus timing and compatibility of the installed provider still need testing on the target machine.

Use [macOS diagnostics](diagnostics.md) to collect the loaded provider identity and first transfer failure. Do not replace a failed addressed request with a different provider ABI.
