#!/bin/bash

set -u
set -o pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'Run this collector on the affected macOS installation.' >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo bash Tools/collect_macos.sh so every capture has the same privileges.' >&2
    exit 1
fi

output=$(mktemp -d "${TMPDIR:-/tmp}/cirrus-evidence.XXXXXX") || exit 1

capture() {
    local name="$1"
    shift

    {
        printf '$'
        printf ' %q' "$@"
        printf '\n\n'
    } >"$output/$name.txt"

    local header_size
    header_size=$(wc -c <"$output/$name.txt")
    "$@" >>"$output/$name.txt" 2>&1

    local result=$?
    printf '%-30s exit=%s\n' "$name" "$result" >>"$output/status.txt"
    if [ "$(wc -c <"$output/$name.txt")" -eq "$header_size" ]; then
        printf '%-30s empty output\n' "$name" >>"$output/status.txt"
    fi

    return "$result"
}

capture_shell() {
    local name="$1"
    shift
    local command="$*"

    {
        printf '$ %s\n\n' "$command"
    } >"$output/$name.txt"

    local header_size
    header_size=$(wc -c <"$output/$name.txt")
    /bin/bash -o pipefail -c "$command" >>"$output/$name.txt" 2>&1

    local result=$?
    printf '%-30s exit=%s\n' "$name" "$result" >>"$output/status.txt"
    if [ "$(wc -c <"$output/$name.txt")" -eq "$header_size" ]; then
        printf '%-30s empty output\n' "$name" >>"$output/status.txt"
    fi

    return "$result"
}

capture system /usr/bin/sw_vers
capture kernel /usr/bin/uname -a
capture boot-time /usr/sbin/sysctl kern.boottime
capture boot-args /usr/sbin/nvram boot-args

capture amplifier-calibration \
    /usr/sbin/nvram \
    '02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData'

capture loaded-kexts /usr/bin/kmutil showloaded
# Keep the unfiltered result. Legacy kextstat is a secondary observation, not
# proof that all OpenCore-injected extensions are absent when it returns empty.
if [ -x /usr/sbin/kextstat ]; then
    capture loaded-kexts-legacy /usr/sbin/kextstat
fi

capture audio /usr/sbin/system_profiler SPAudioDataType
capture pci /usr/sbin/system_profiler SPPCIDataType
capture services /usr/sbin/ioreg -lw0 -p IOService
capture acpi /usr/sbin/ioreg -lw0 -p IOACPIPlane
capture power-plane /usr/sbin/ioreg -lw0 -p IOPower

capture cirrus-fixup-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c CirrusAudioFixup
capture voodoo-i2c-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c VoodooI2CDeviceNub
capture hda-controller-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c AppleHDAController
capture hda-driver-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c AppleHDA

capture_shell ioreg-cirrus '
/usr/sbin/ioreg -lw0 |
/usr/bin/grep -i -C 20 -E "Cirrus|CS35L41|CSC3551|AppleALC|HDA|Audio"
'

capture sysctl-msgbuf /usr/sbin/sysctl -n kern.msgbuf

capture dmesg /sbin/dmesg

printf '%s\n' \
    'Optional queries may be unavailable; inspect each exit code and raw output.' \
    'ioreg may exit 0 without a matching service. That is not a health verdict.' \
    'kmutil shows extension load state; it does not prove an audio route works.' \
    'Unified logs and kernel buffers may not retain early boot messages.' \
    'AppleALC is checked in loaded-kexts.txt, not as an AppleALC IOService class.' \
    >"$output/README.txt"

kernel_log="$output/kernel-log.txt"
if ! capture kernel-log /usr/bin/log show --last boot --style syslog --info --debug \
    --predicate 'process == "kernel"'; then
    # Preserve the failed since-boot query and try a bounded interval separately.
    capture kernel-log-fallback /usr/bin/log show --last 1h --style syslog --info --debug \
        --predicate 'process == "kernel"'
    kernel_log="$output/kernel-log-fallback.txt"
fi
capture cirrus-audio-log /usr/bin/grep -Ei \
    'Cirrus|CS35|CSC3551|AppleALC|Lilu|Voodoo|SPKR' "$kernel_log"

if [ "$#" -gt 0 ]; then
    kext="$1"

    printf '%s\n' "$kext" >"$output/kext-path.txt"

    if [ ! -d "$kext" ]; then
        printf 'KEXT bundle does not exist: %s\n' "$kext" >"$output/kext-error.txt"
    else
        binary="$kext/Contents/MacOS/CirrusAudioFixup"

        capture bundle /usr/bin/plutil -p "$kext/Contents/Info.plist"
        capture bundle-id /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$kext/Contents/Info.plist"
        capture bundle-version /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$kext/Contents/Info.plist"
        capture bundle-signature /usr/bin/codesign -dv --verbose=4 "$kext"

        if [ -f "$binary" ]; then
            capture binary-file /usr/bin/file "$binary"
            capture binary-sha256 /usr/bin/shasum -a 256 "$binary"
            capture binary-uuid /usr/bin/dwarfdump --uuid "$binary"
            capture binary-imports /usr/bin/nm -u "$binary"
        else
            printf 'Binary does not exist: %s\n' "$binary" >"$output/binary-error.txt"
        fi
    fi
fi

printf '\nEvidence saved: %s\n' "$output"
printf 'Check: %s/status.txt\n' "$output"
printf '\nImportant files:\n'
printf '  %s/sysctl-msgbuf.txt\n' "$output"
printf '  %s/dmesg.txt\n' "$output"
printf '  %s/cirrus-audio-log.txt\n' "$output"
printf '  %s/cirrus-fixup-ioreg.txt\n' "$output"
printf '  %s/voodoo-i2c-ioreg.txt\n' "$output"
