# Corrected board-identity retry build — 2026-10-13

The first test boot loaded the October 12 Debug bundle but stayed silent because
HDA PCI identity was absent at early service attachment. The corrected Debug
bundle from commit `e0b46ee` is now installed into the same EFI path.

- Expected new UUID: `B0FF6C51-3DDC-3E0D-899B-03743BD14534`.
- Executable SHA-256: `cd53c2f093a9e39f856706309bf8c9fb431f72d0448438cd500766005db7d990`.
- Backup and preserved failing logs:
  `build/EFIDeployment/20261013-013712-e0b46ee-Debug`.
- Config, boot arguments, NVRAM and other kexts remain unchanged.
- Debug now prints `Author: Tran Kinh Hoang (hoaugtr)` and distinguishes
  BOARD_IDENTITY_WAIT / BOARD_IDENTITY_RESOLVED from policy rejection.
- The current kernel still runs UUID `6670947A-557E-3938-8046-AFD36E3CC12A`;
  the corrected bundle needs another boot. Audible output is not yet confirmed.

Use this new UUID for the next boot checks. The October 12 deployment record below
is historical. The new backup contains the silent test version; the original
known-working pre-refactor bundle remains backed up in
`build/EFIDeployment/20261012-235422-7a8c46e-Debug/CirrusAudioFixup.before.kext`.

# Verified EFI Debug deployment — 2026-10-12

The Debug bundle built from source commit `7a8c46e` is installed at
`/Volumes/EFI/EFI/OC/Kexts/CirrusAudioFixup.kext`. Debug enables verbose diagnostics
by default; no additional `-cirrusdbg` flag is needed. This is a boot-test build.
The separately verified optimized Release bundle remains in `build/Local-Release`.

| Artifact | Evidence |
| --- | --- |
| Installed Debug version | 1.0.1 |
| Installed Debug UUID | `6670947A-557E-3938-8046-AFD36E3CC12A` |
| Installed Debug executable SHA-256 | `74bcf8e5879ae2231a4bf6e65068e4c62a184407c01b460bbc1d03b1061fc754` |
| Previous / currently loaded UUID at deployment | `2564C407-4ED3-379A-8693-8C529B52434D` |
| Previous executable SHA-256 | `eac87c472f9731acde14584156df7610c862389a2dfb187ae4fd7ec5c46a528f` |
| Release UUID | `3D144221-8F67-3AC0-B533-356EF4AF9294` |
| Release executable SHA-256 | `6bad5a529b80d6c16b1a9be0add9e04d6d59b300e8ee12572622738d5dd392a4` |

The backup directory is
`build/EFIDeployment/20261012-235422-7a8c46e-Debug`. It contains the verified
`CirrusAudioFixup.before.kext`, `config.before.plist`, matching new Debug dSYM,
deployment manifest and read-only current-kernel evidence. The manifest at
`build/EFIDeployment/latest.json` records per-file hashes, original/new binary
identities and the unchanged configuration hash.

## Completed preparation

- 28 maintained host suites plus the optional Windows audit passed (29 total).
- Normal/sanitizer driver and communication fault harnesses passed.
- All 74 production files passed the pinned formatter.
- Debug and Release builds passed plist/metadata, OpenCore vtable, x86_64 and
  matching binary/dSYM UUID gates. Existing SDK kmod deployment-floor warnings
  are unchanged.
- All enabled OpenCore kext bundle/executable/plist paths exist. The Cirrus entry
  is enabled, has no kernel-version exclusion and uses the correct paths.
- Installed Lilu 1.7.2 matches the bundle dependency. The installed custom
  VoodooI2C executable matches the audited hash/loaded UUID in
  [provider evidence](provider_abi_evidence.md).
- The old EFI bundle matched the currently loaded Cirrus UUID. The new bundle
  was staged and verified before replacement, with rollback on verification
  failure. Every installed file matches the built bundle.
- OpenCore config, boot arguments, NVRAM and other kexts were not changed. No live
  kext reload, reboot or hardware output test was performed during deployment.

## First boot checks

1. Reboot through this OpenCore EFI and confirm `kmutil showloaded` reports
   Cirrus UUID `6670947A-557E-3938-8046-AFD36E3CC12A`. A different UUID means the
   intended bundle has not been loaded; do not attribute that boot's behavior to
   this build.
2. Inspect `ioreg -lw0 -r -c CirrusAudioFixup`: build configuration should be Debug,
   debug enabled, communication/AMP/board identities consistent, and both verified
   endpoint lifecycles ready. Check first/latest attachment and diagnostic faults,
   native returns, firmware readiness and output route.
3. Test audible left/right speakers, playback start/stop, headphone insertion and
   removal, then repeated sleep/wake and a cold boot. Record which transition
   failed rather than relying on READY_DSP alone.
4. Collect evidence from the repository with
   `sudo bash Tools/collect_macos.sh`. Preserve the reported output directory and
   its `status.txt` so capture failures are visible. Retain the matching Debug
   dSYM from the backup with any panic/log evidence.
5. After those tests pass, use the verified Release bundle for quiet operation.
   Release can enable verbose diagnostics with `-cirrusdbg` when investigation
   is needed. Physical performance and latency need real-machine measurements.

If rollback is needed, restore `CirrusAudioFixup.before.kext` from the backup to
this same EFI bundle path, verify its old SHA-256 and reboot through that EFI.
The config backup is available but was not modified by this deployment.

This records preparation and installed bytes, not completed new-build hardware
validation. Cold boot, audible output, routing and sleep/wake remain pending until
the new UUID is observed running and those flows are exercised.
