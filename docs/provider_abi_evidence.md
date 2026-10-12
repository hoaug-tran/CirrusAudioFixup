# Custom VoodooI2C provider evidence — 2026-10-12

Read-only inspection of this machine and `docs/Kexts.zip` establishes a concrete
binary contract for the existing adapter. It does not establish the fork's source
revision, redistribution rights or physical bus timing.

| Artifact | Version | Loaded UUID / archive UUID | Archive executable SHA-256 |
| --- | --- | --- | --- |
| `com.alexandred.VoodooI2C` | 2.9.1 | `379C7711-B1CA-3D28-B9D5-A4E5A311894D` | `28fa3a0cb332572c32ef0bce5ba6a53191dc4a819880cb39526c91e4be04d011` |
| `com.alexandred.VoodooI2CServices` | 1 | `0664384E-DA20-35E5-8FBC-07B5389CE426` | `a4cd7fc9fb198fd0c0106ad7316cf7e71c8071dd0667ebcc7ff26aa61036ffff` |

`kmutil showloaded --list-only` reported those versions and UUIDs. `dwarfdump
--uuid` on the executables extracted to a temporary directory from the archive
reported the same UUIDs. This identifies matching binary candidates; no hash of
loaded kernel memory was obtained.

`nm -C`, `strings` and `otool -tvV` show that
`VoodooI2CDeviceNub::callPlatformFunction(OSSymbol const*, bool, void*, ...)`
compares the symbol with `VoodooI2CTransferToAddress` and passes the first argument
to `transferI2CToAddress(VoodooI2CAddressedTransfer*)`. Unmatched symbols delegate
to the superclass path.

The x86_64 handler reads the following offsets. Compile-time assertions now pin
the adapter's 40-byte, 8-byte-aligned request layout.

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 1 byte | Address; negative sign-extended values reject addresses above 0x7f |
| 8 | 8 bytes | Write-buffer pointer |
| 16 | 2 bytes | Write length |
| 24 | 8 bytes | Read-buffer pointer |
| 32 | 2 bytes | Read length |

When both pointer/length pairs are nonzero, the handler constructs two controller
messages with the same requested address and calls `transferI2C(messages, 2)`.
For a single direction it uses one message. The native return is returned to the
caller. This agrees with the production adapter's addressed request and combined
read model. It does not measure repeated-START on the wire, arbitration, timeout
or cancellation behavior inside the controller.

The exact fork URL/revision, source-to-binary reproducibility and distribution
license remain absent. No provider code was imported and `LICENSE` was not
changed. The stock provider ABI must not be substituted on the strength of this
custom binary evidence. Other controller paths retain their separate evidence
gates.
