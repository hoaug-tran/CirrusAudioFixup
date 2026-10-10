#!/bin/bash
# Build and verify the current checkout without installing or loading its kext.
# Run with: bash Tools/build_macos.sh
# Xcode, Python 3 and the repository's pinned SDK/Lilu checkouts are required.
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "error: a macOS host with Xcode is required; no bootable artifact produced" >&2
    exit 1
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
for command in xcodebuild python3 plutil lipo dwarfdump shasum; do
    command -v "$command" >/dev/null || { echo "error: missing $command" >&2; exit 1; }
done
for dependency in MacKernelSDK/Headers/IOKit/IOService.h Lilu/Lilu/Headers/kern_patcher.hpp; do
    [[ -f "$dependency" ]] || { echo "error: missing dependency $dependency" >&2; exit 1; }
done

# Verify the source and real embedded payloads before spending time on Xcode.
for test in Tests/check_*.py; do
    python3 "$test"
done
python3 Tests/reproduce_host.py
python3 Tools/release_metadata.py

for mode in Debug Release; do
    xcodebuild -project CirrusAudioFixup.xcodeproj -target CirrusAudioFixup \
        -configuration "$mode" -sdk macosx \
        CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO \
        CONFIGURATION_BUILD_DIR="$root/build/Local-$mode" build
    bundle="$root/build/Local-$mode/CirrusAudioFixup.kext"
    binary="$bundle/Contents/MacOS/CirrusAudioFixup"
    symbols="$bundle.dSYM/Contents/Resources/DWARF/CirrusAudioFixup"
    plutil -lint "$bundle/Contents/Info.plist"
    python3 Tools/release_metadata.py --bundle "$bundle"
    lipo "$binary" -verify_arch x86_64
    [[ -f "$symbols" ]] || { echo "error: missing symbols for $mode" >&2; exit 1; }
    binary_uuid="$(dwarfdump --uuid "$binary" | awk '{print $2}')"
    symbols_uuid="$(dwarfdump --uuid "$symbols" | awk '{print $2}')"
    [[ -n "$binary_uuid" && "$binary_uuid" == "$symbols_uuid" ]] || {
        echo "error: binary/symbol UUID mismatch for $mode" >&2; exit 1;
    }
    shasum -a 256 "$binary"
    echo "PASS $mode bundle: $bundle"
done
echo "Build verification passed. Hardware playback and load compatibility are not yet verified."
