#!/bin/bash

set -u

if [ "$(uname -s)" != "Darwin" ]; then
    printf '%s\n' 'Run this collector on the affected macOS installation.' >&2
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

#
# Environment
#

capture system /usr/bin/sw_vers
capture kernel /usr/bin/uname -a
capture boot-time /usr/sbin/sysctl kern.boottime
capture boot-args /usr/sbin/nvram boot-args

capture paths /bin/bash -c '
echo "PATH=$PATH"
for x in \
    /sbin/dmesg \
    /usr/bin/log \
    /usr/bin/kmutil \
    /usr/sbin/kextstat \
    /usr/sbin/ioreg \
    /usr/sbin/system_profiler
do
    if [ -e "$x" ]; then
        ls -l "$x"
    else
        echo "MISSING: $x"
    fi
done
'

#
# Cirrus NVRAM
#

capture amplifier-calibration \
    /usr/sbin/nvram \
    '02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData'

#
# KEXT state
#

capture loaded-kexts \
    /usr/bin/kmutil showloaded

capture loaded-kexts-all \
    /usr/bin/kmutil showloaded --show all

capture loaded-kexts-aux \
    /usr/bin/kmutil showloaded --collection aux --show all

capture loaded-kexts-boot \
    /usr/bin/kmutil showloaded --collection boot --show all

capture loaded-kexts-system \
    /usr/bin/kmutil showloaded --collection sys --show all

capture loaded-kexts-codeless \
    /usr/bin/kmutil showloaded --collection codeless --show all

#
# Specifically look for our audio stack
#

capture_shell kext-audio-filter '
/usr/bin/kmutil showloaded --show all 2>&1 |
/usr/bin/grep -Ei "Cirrus|CS35|AppleALC|Lilu|WhateverGreen|Voodoo|Audio"
'

#
# IORegistry
#

capture audio \
    /usr/sbin/system_profiler SPAudioDataType

capture services \
    /usr/sbin/ioreg -lw0 -p IOService

capture acpi \
    /usr/sbin/ioreg -lw0 -p IOACPIPlane

capture power-plane \
    /usr/sbin/ioreg -lw0 -p IOPower

capture_shell ioreg-cirrus '
/usr/sbin/ioreg -lw0 |
/usr/bin/grep -i -C 20 -E "Cirrus|CS35L41|CSC3551|AppleALC|HDA|Audio"
'

#
# dmesg
#
# dmesg is /sbin/dmesg on macOS and normally requires root.
#

if [ -x /sbin/dmesg ]; then
    if [ "$(id -u)" -eq 0 ]; then
        capture dmesg /sbin/dmesg
    else
        capture dmesg sudo /sbin/dmesg
    fi
else
    printf 'dmesg: /sbin/dmesg does not exist\n' \
        >"$output/dmesg.txt"
    printf '%-30s exit=%s\n' "dmesg" "127" >>"$output/status.txt"
fi

#
# Unified Logging
#
# --info and --debug are important because `log show`
# normally only includes default-level messages.
#

capture kernel-log \
    /usr/bin/log show \
    --last boot \
    --style syslog \
    --info \
    --debug \
    --predicate 'process == "kernel"'

capture kernel-sender-log \
    /usr/bin/log show \
    --last boot \
    --style syslog \
    --info \
    --debug \
    --predicate 'process == "kernel" OR senderImagePath CONTAINS[c] "kernel"'

capture cirrus-log \
    /usr/bin/log show \
    --last boot \
    --style syslog \
    --info \
    --debug \
    --predicate \
    'eventMessage CONTAINS[c] "CirrusAudioFixup"
     OR senderImagePath CONTAINS[c] "CirrusAudioFixup"
     OR eventMessage CONTAINS[c] "CS35"
     OR eventMessage CONTAINS[c] "Cirrus"
     OR eventMessage CONTAINS[c] "CSC3551"'

capture audio-driver-log \
    /usr/bin/log show \
    --last boot \
    --style syslog \
    --info \
    --debug \
    --predicate \
    'eventMessage CONTAINS[c] "Cirrus"
     OR eventMessage CONTAINS[c] "CS35"
     OR eventMessage CONTAINS[c] "AppleALC"
     OR eventMessage CONTAINS[c] "Lilu"
     OR eventMessage CONTAINS[c] "VoodooI2C"
     OR senderImagePath CONTAINS[c] "AppleALC"
     OR senderImagePath CONTAINS[c] "Lilu"'

#
# Full boot log
#

capture system-log-full \
    /usr/bin/log show \
    --last boot \
    --style syslog \
    --info \
    --debug

#
# Optional KEXT bundle passed as argv[1]
#

if [ "$#" -gt 0 ]; then
    kext="$1"

    printf '%s\n' "$kext" >"$output/kext-path.txt"

    if [ ! -d "$kext" ]; then
        printf 'KEXT bundle does not exist: %s\n' "$kext" \
            >"$output/kext-error.txt"
    else
        binary="$kext/Contents/MacOS/CirrusAudioFixup"

        capture bundle \
            /usr/bin/plutil -p "$kext/Contents/Info.plist"

        capture bundle-id \
            /usr/libexec/PlistBuddy \
            -c 'Print :CFBundleIdentifier' \
            "$kext/Contents/Info.plist"

        capture bundle-version \
            /usr/libexec/PlistBuddy \
            -c 'Print :CFBundleVersion' \
            "$kext/Contents/Info.plist"

        capture bundle-signature \
            /usr/bin/codesign -dv --verbose=4 "$kext"

        if [ -f "$binary" ]; then
            capture binary-file \
                /usr/bin/file "$binary"

            capture binary-sha256 \
                /usr/bin/shasum -a 256 "$binary"

            capture binary-uuid \
                /usr/bin/dwarfdump --uuid "$binary"

            capture binary-imports \
                /usr/bin/nm -u "$binary"
        else
            printf 'Binary does not exist: %s\n' "$binary" \
                >"$output/binary-error.txt"
        fi
    fi
fi

#
# Summary
#

printf '\nEvidence saved: %s\n' "$output"
printf 'Check: %s/status.txt\n' "$output"

printf '\nImportant files:\n'
printf '  %s/dmesg.txt\n' "$output"
printf '  %s/loaded-kexts-all.txt\n' "$output"
printf '  %s/loaded-kexts-aux.txt\n' "$output"
printf '  %s/kext-audio-filter.txt\n' "$output"
printf '  %s/cirrus-log.txt\n' "$output"
printf '  %s/audio-driver-log.txt\n' "$output"