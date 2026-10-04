#!/bin/bash

set -u

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'Run this collector on the affected macOS installation.' >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    if [ -t 0 ]; then
        sudo -v
    fi
fi

output=$(mktemp -d "${TMPDIR:-/tmp}/cirrus-evidence.XXXXXX") || exit 1

capture() {
    local name="$1"
    shift

    {
        printf '$'
        printf ' %q' "$@"
        printf '\n\n'
        "$@"
    } >"$output/$name.txt" 2>&1

    local result=$?
    printf '%-30s exit=%s\n' "$name" "$result" >>"$output/status.txt"

    return 0
}

capture_shell() {
    local name="$1"
    shift
    local command="$*"

    {
        printf '$ %s\n\n' "$command"
        /bin/bash -c "$command"
    } >"$output/$name.txt" 2>&1

    local result=$?
    printf '%-30s exit=%s\n' "$name" "$result" >>"$output/status.txt"

    return 0
}

capture system /usr/bin/sw_vers
capture kernel /usr/bin/uname -a
capture boot-time /usr/sbin/sysctl kern.boottime
capture boot-args /usr/sbin/nvram boot-args

capture amplifier-calibration \
    /usr/sbin/nvram \
    '02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData'

capture loaded-kexts /usr/bin/kmutil showloaded
capture loaded-kexts-all /usr/bin/kmutil showloaded --show all
capture loaded-kexts-aux /usr/bin/kmutil showloaded --collection aux --show all
capture loaded-kexts-boot /usr/bin/kmutil showloaded --collection boot --show all
capture loaded-kexts-system /usr/bin/kmutil showloaded --collection sys --show all
capture loaded-kexts-codeless /usr/bin/kmutil showloaded --collection codeless --show all

capture_shell kext-audio-filter '
/usr/bin/kmutil showloaded --show all 2>&1 |
/usr/bin/grep -Ei "Cirrus|CS35|AppleALC|Lilu|WhateverGreen|Voodoo|Audio"
'

capture audio /usr/sbin/system_profiler SPAudioDataType
capture pci /usr/sbin/system_profiler SPPCIDataType
capture services /usr/sbin/ioreg -lw0 -p IOService
capture acpi /usr/sbin/ioreg -lw0 -p IOACPIPlane
capture power-plane /usr/sbin/ioreg -lw0 -p IOPower

capture cirrus-fixup-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c CirrusAudioFixup
capture cs35l41-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c CS35L41
capture voodoo-i2c-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c VoodooI2CDeviceNub
capture apple-alc-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c AppleALC
capture hda-controller-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c AppleHDAController

capture_shell ioreg-cirrus '
/usr/sbin/ioreg -lw0 |
/usr/bin/grep -i -C 20 -E "Cirrus|CS35L41|CSC3551|AppleALC|HDA|Audio"
'

capture sysctl-msgbuf /usr/sbin/sysctl -n kern.msgbuf

if [ "$(id -u)" -eq 0 ]; then
    capture dmesg /sbin/dmesg
else
    capture dmesg sudo /sbin/dmesg
fi

if [ "$(id -u)" -eq 0 ]; then
    capture_shell kernel-log '
/usr/bin/log show --last boot --style syslog --info --debug --predicate '\''process == "kernel"'\''
'
    capture_shell cirrus-audio-log '
/usr/bin/log show --last boot --style syslog --info --debug --predicate '\''process == "kernel"'\'' |
/usr/bin/grep -Ei "Cirrus|CS35|CSC3551|AppleALC|Lilu|Voodoo|SPKR"
'
else
    capture_shell kernel-log '
sudo /usr/bin/log show --last boot --style syslog --info --debug --predicate '\''process == "kernel"'\''
'
    capture_shell cirrus-audio-log '
sudo /usr/bin/log show --last boot --style syslog --info --debug --predicate '\''process == "kernel"'\'' |
/usr/bin/grep -Ei "Cirrus|CS35|CSC3551|AppleALC|Lilu|Voodoo|SPKR"
'
fi

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