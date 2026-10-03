#!/bin/bash
set -u
if [ "$(uname -s)" != Darwin ]; then
    printf '%s\n' 'Run this collector on the affected macOS installation.' >&2
    exit 1
fi
output=$(mktemp -d "${TMPDIR:-/tmp}/cirrus-evidence.XXXXXX") || exit 1
capture() {
    local name=$1
    shift
    "$@" >"$output/$name.txt" 2>&1
    local result=$?
    printf '%s exit=%s\n' "$name" "$result" >>"$output/status.txt"
}
capture system sw_vers
capture kernel uname -a
capture boot-time sysctl kern.boottime
capture boot-args nvram boot-args
capture amplifier-calibration nvram '02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData'
capture loaded-kexts kmutil showloaded
capture audio system_profiler SPAudioDataType
capture services ioreg -lw0 -p IOService
capture acpi ioreg -lw0 -p IOACPIPlane
capture power-plane ioreg -lw0 -p IOPower
capture cirrus-log log show --last boot --style syslog --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
if [ "$#" -gt 0 ]; then
    kext=$1
    binary="$kext/Contents/MacOS/CirrusAudioFixup"
    capture bundle plutil -p "$kext/Contents/Info.plist"
    capture binary-file file "$binary"
    capture binary-sha256 shasum -a 256 "$binary"
    capture binary-uuid dwarfdump --uuid "$binary"
    capture binary-imports nm -u "$binary"
    capture binary-signature codesign -dv --verbose=4 "$kext"
fi
printf 'Evidence saved: %s\nCheck status.txt for commands needing elevated access.\n' "$output"
