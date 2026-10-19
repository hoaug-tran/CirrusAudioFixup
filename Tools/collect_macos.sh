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

# sudo may set HOME to /var/root. Resolve the invoking account through Directory
# Services instead; direct root execution uses the logged-in console account.
evidence_user="${SUDO_USER:-}"
if [ -z "$evidence_user" ] || [ "$evidence_user" = root ]; then
    evidence_user=$(/usr/bin/stat -f '%Su' /dev/console) || exit 1
fi
if [ -z "$evidence_user" ] || [ "$evidence_user" = root ] || [ "$evidence_user" = loginwindow ]; then
    printf '%s\n' 'No desktop user found. Run this collector with sudo from your user session.' >&2
    exit 1
fi
evidence_uid=$(/usr/bin/id -u "$evidence_user") || exit 1
evidence_gid=$(/usr/bin/id -g "$evidence_user") || exit 1
evidence_home_record=$(/usr/bin/dscl . -read "/Users/$evidence_user" NFSHomeDirectory) || exit 1
evidence_home="${evidence_home_record#NFSHomeDirectory: }"
case "$evidence_home" in
    /*) ;;
    *) printf '%s\n' 'Could not resolve the desktop user home directory.' >&2; exit 1 ;;
esac
evidence_desktop="$evidence_home/Desktop"
if [ ! -d "$evidence_desktop" ]; then
    /bin/mkdir -p "$evidence_desktop" || exit 1
    /usr/sbin/chown "$evidence_uid:$evidence_gid" "$evidence_desktop" || exit 1
    /bin/chmod u+rwx "$evidence_desktop" || exit 1
fi
umask 077
output=$(/usr/bin/mktemp -d "$evidence_desktop/cirrus-evidence.XXXXXX") || exit 1
archive="$output.zip"
capture_failed=0
collection_finished=0

# Return partial captures too, including interrupted/error exits. Only this new
# evidence directory is modified; existing Desktop files keep their permissions.
finish_evidence() {
    local result=$?
    trap - EXIT
    if [ "$collection_finished" -ne 1 ]; then
        printf 'INTERRUPTED exit=%s\n' "$result" >>"$output/status.txt"
    fi
    # Package as the recipient: ZIP entries and the ZIP itself must not inherit
    # root-only ownership. Keep the directory even if packaging fails.
    if ! /usr/sbin/chown -R "$evidence_uid:$evidence_gid" "$output" ||
       ! /bin/chmod -R u+rwX "$output"; then
        printf 'Could not grant user access to evidence: %s\n' "$output" >&2
        result=1
    fi
    if /usr/bin/sudo -u "$evidence_user" /usr/bin/ditto -c -k --keepParent "$output" "$archive"; then
        if /usr/sbin/chown "$evidence_uid:$evidence_gid" "$archive" &&
           /bin/chmod 600 "$archive" &&
           /usr/bin/sudo -u "$evidence_user" /usr/bin/unzip -tq "$archive"; then
            printf '\nSend: %s\n' "$archive"
        else
            printf 'Archive access/integrity check failed: %s\n' "$archive" >&2
            result=1
        fi
    else
        printf 'ZIP failed; evidence remains at: %s\n' "$output" >&2
        result=1
    fi
    if [ "$result" -eq 0 ] && [ "$capture_failed" -ne 0 ]; then result=2; fi
    exit "$result"
}
trap finish_evidence EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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
    printf 'Capturing %s\n' "$name"
    # macOS ships no timeout(1). Bound each query; a stuck diagnostic must not
    # prevent subsequent captures or the EXIT ownership repair.
    "$@" >>"$output/$name.txt" 2>&1 &
    local command_pid=$!
    (
        sleep 120 &
        timer_pid=$!
        trap 'kill "$timer_pid" 2>/dev/null; exit' TERM INT
        wait "$timer_pid"
        if kill -0 "$command_pid" 2>/dev/null; then
            printf '\nTIMEOUT after 120 seconds\n' >>"$output/$name.txt"
            kill -TERM "$command_pid" 2>/dev/null
            sleep 2
            kill -KILL "$command_pid" 2>/dev/null
        fi
    ) &
    local watchdog_pid=$!
    wait "$command_pid"
    local result=$?
    kill "$watchdog_pid" 2>/dev/null
    wait "$watchdog_pid" 2>/dev/null
    if [ "$result" -ne 0 ]; then capture_failed=1; fi
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

    capture "$name" /bin/bash -o pipefail -c "$command"
}

capture system /usr/bin/sw_vers
capture collector-sha256 /usr/bin/shasum -a 256 "${BASH_SOURCE[0]}"
capture collected-at /bin/date '+%Y-%m-%dT%H:%M:%S%z'
capture kernel /usr/bin/uname -a
capture cpu /usr/sbin/sysctl machdep.cpu.vendor machdep.cpu.brand_string
capture machine-model /usr/sbin/sysctl hw.model
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
capture services-xml /usr/sbin/ioreg -a -l -p IOService

capture cirrus-fixup-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c CirrusAudioFixup
capture cirrus-bootstrap-ioreg /usr/sbin/ioreg -lw0 -p IOService -r -c CirrusAudioBootstrap -d 1
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
boot_seconds=$(/usr/sbin/sysctl -n kern.boottime | /usr/bin/sed -E 's/.*sec = ([0-9]+),.*/\1/')
boot_start=$(/bin/date -r "$boot_seconds" '+%Y-%m-%d %H:%M:%S' 2>/dev/null) || boot_start=''
log_predicate='process == "kernel" AND (eventMessage CONTAINS[c] "Cirrus" OR eventMessage CONTAINS[c] "CS35" OR eventMessage CONTAINS[c] "CSC355" OR eventMessage CONTAINS[c] "BOOT_FLOW" OR eventMessage CONTAINS[c] "AppleALC" OR eventMessage CONTAINS[c] "Lilu" OR eventMessage CONTAINS[c] "Voodoo" OR eventMessage CONTAINS[c] "AppleHDA" OR eventMessage CONTAINS[c] "SPI" OR eventMessage CONTAINS[c] "panic" OR eventMessage CONTAINS[c] "watchdog")'
if [ -z "$boot_start" ] || ! capture kernel-log /usr/bin/log show --start "$boot_start" --style syslog --info --debug \
    --predicate "$log_predicate"; then
    # Preserve the failed since-boot query and try a bounded interval separately.
    capture kernel-log-fallback /usr/bin/log show --last 1h --style syslog --info --debug \
        --predicate "$log_predicate"
    kernel_log="$output/kernel-log-fallback.txt"
fi

printf 'collector_version=2\nstarted_boot=%s\nrecipient=%s\n' "$boot_start" "$evidence_user" >"$output/collection.txt"
printf '%s\n' 'Collection reads existing diagnostics only; it does not load/unload kexts or probe amplifier registers.' \
    'A service query can succeed without matching a device. Inspect IORegistry and status together.' \
    'Exit 2 means one or more optional captures failed; send the partial ZIP too.' \
    'A kext path identifies the supplied bundle, not proof that this exact binary is loaded.' >>"$output/README.txt"
capture cirrus-audio-log /usr/bin/grep -Ei \
    'Cirrus|CS35|CSC3551|AppleALC|Lilu|Voodoo|SPKR' "$kernel_log"

if [ "$#" -gt 0 ]; then
    kext="$1"

    printf '%s\n' "$kext" >"$output/kext-path.txt"

    if [ ! -d "$kext" ]; then
        printf 'KEXT bundle does not exist: %s\n' "$kext" >"$output/kext-error.txt"
        printf 'bundle exit=1 missing supplied bundle\n' >>"$output/status.txt"
        capture_failed=1
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
            printf 'binary exit=1 missing supplied binary\n' >>"$output/status.txt"
            capture_failed=1
        fi
    fi
else
    printf '%s\n' 'NOT_PROVIDED: pass the CirrusAudioFixup.kext path as the first argument to capture bundle metadata, hash and UUID.' >"$output/kext-error.txt"
    printf 'bundle NOT_PROVIDED\n' >>"$output/status.txt"
    capture_failed=1
fi

printf '\nEvidence saved: %s\n' "$output"
printf 'Check: %s/status.txt\n' "$output"
printf '\nImportant files:\n'
printf '  %s/sysctl-msgbuf.txt\n' "$output"
printf '  %s/dmesg.txt\n' "$output"
printf '  %s/cirrus-audio-log.txt\n' "$output"
printf '  %s/cirrus-fixup-ioreg.txt\n' "$output"
printf '  %s/voodoo-i2c-ioreg.txt\n' "$output"
collection_finished=1
