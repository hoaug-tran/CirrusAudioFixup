#!/bin/bash
# Build and verify the current checkout without installing or loading its kext.
# Run with: bash Tools/Build/build_macos.sh
# Xcode, Python 3 and the repository's pinned SDK/Lilu checkouts are required.
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "error: a macOS host with Xcode is required; no bootable artifact produced" >&2
    exit 1
fi

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
for command in git xcodebuild python3 plutil lipo dwarfdump shasum; do
    command -v "$command" >/dev/null || { echo "error: missing $command" >&2; exit 1; }
done
for dependency in MacKernelSDK/Headers/IOKit/IOService.h Lilu/Lilu/Headers/kern_patcher.hpp; do
    [[ -f "$dependency" ]] || { echo "error: missing dependency $dependency" >&2; exit 1; }
done

# Verify the source and real embedded payloads before spending time on Xcode.
python3 Tests/run.py
python3 Tools/Build/release_metadata.py

provenance_snapshot="$(mktemp "${TMPDIR:-/tmp}/cirrus-build-source.XXXXXX")"
trap 'rm -f "$provenance_snapshot"' EXIT
python3 Tools/Build/build_provenance.py snapshot --root "$root" --snapshot "$provenance_snapshot"

for mode in Debug Release; do
    bundle="$root/build/Local-$mode/CirrusAudioFixup.kext"
    # A failed rebuild must not leave an earlier source claim on a new binary.
    rm -f "$bundle/Contents/Resources/CirrusBuildManifest.plist"
    xcodebuild -project CirrusAudioFixup.xcodeproj -target CirrusAudioFixup \
        -configuration "$mode" -sdk macosx \
        CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO \
        CONFIGURATION_BUILD_DIR="$root/build/Local-$mode" build
    binary="$bundle/Contents/MacOS/CirrusAudioFixup"
    symbols="$bundle.dSYM/Contents/Resources/DWARF/CirrusAudioFixup"
    plutil -lint "$bundle/Contents/Info.plist"
    python3 Tools/Build/release_metadata.py --bundle "$bundle"
    python3 Tools/Build/verify_kext_vtables.py "$binary"
    lipo "$binary" -verify_arch x86_64
    [[ -f "$symbols" ]] || { echo "error: missing symbols for $mode" >&2; exit 1; }
    binary_uuid="$(dwarfdump --uuid "$binary" | awk '{print $2}')"
    symbols_uuid="$(dwarfdump --uuid "$symbols" | awk '{print $2}')"
    [[ -n "$binary_uuid" && "$binary_uuid" == "$symbols_uuid" ]] || {
        echo "error: binary/symbol UUID mismatch for $mode" >&2; exit 1;
    }
    shasum -a 256 "$binary"
    python3 Tools/Build/build_provenance.py finalize --root "$root" --snapshot "$provenance_snapshot" \
        --bundle "$bundle" --configuration "$mode" \
        --binary-uuid "$binary_uuid" --symbols-uuid "$symbols_uuid"
    plutil -lint "$bundle/Contents/Info.plist" "$bundle/Contents/Resources/CirrusBuildManifest.plist"
    echo "PASS $mode bundle: $bundle"
done
echo "Build verification passed. Hardware playback and load compatibility are not yet verified."
