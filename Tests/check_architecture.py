from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text(encoding="utf-8")

cpp = r'''
#include <cassert>
#include <cstring>
#include "Core/DeviceRegistry.hpp"
#include "Platform/PlatformProfile.hpp"

int main() {
    using namespace cirrus;

    const auto* cs35l41 = core::findDeviceById(0x00035A40);
    assert(cs35l41);
    assert(cs35l41->model == core::CodecModel::CS35L41);
    assert(cs35l41->hasHaloDsp);
    assert(core::findDeviceById(0x00000000) == nullptr);
    assert(core::supportsDevice(core::CodecModel::CS35L41));
    assert(!core::supportsDevice(core::CodecModel::CS35L56));

    const auto* lenovo = platform::findPlatformProfile("CLSA0100");
    assert(lenovo);
    assert(lenovo->amplifierModel == core::CodecModel::CS35L41);
    assert(lenovo->endpointCount == 4);
    assert(lenovo->endpoints[0].address == 0x40);
    assert(lenovo->endpoints[1].address == 0x41);
    assert(lenovo->resetQuirk);
    assert(lenovo->allowAutomaticInitialization);
    assert(std::strcmp(lenovo->resetQuirk->controllerName, "AMDI0030") == 0);

    const auto* generic = platform::findPlatformProfile("CSC3551");
    assert(generic);
    assert(generic->resetQuirk == nullptr);
    assert(!generic->allowAutomaticInitialization);
    assert(platform::findPlatformProfile("UNKNOWN") == nullptr);
    const auto& unknown = platform::defaultPlatformProfile();
    assert(unknown.resetQuirk == nullptr);
    assert(!unknown.allowAutomaticInitialization);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cirrus-architecture-check-") as directory:
    tmp = Path(directory)
    source = tmp / "architecture.cpp"
    binary = tmp / "architecture.exe"
    source.write_text(cpp, encoding="utf-8")
    subprocess.run(
        ["g++", "-std=c++17", "-O0", "-I", str(ROOT / "CirrusAudioFixup"), str(source), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

detect_start = SOURCE.index("size_t CirrusAudioFixup::detectAmplifiers()")
detect_end = SOURCE.index("\n}", detect_start)
detect = SOURCE[detect_start:detect_end]
assert 'bootArgEnabled("-cirruslegacyprobe")' in detect
assert "findDeviceById(devId)" in detect
assert "profile.allowAutomaticInitialization || explicitLegacyProbe" in detect
assert "mAmps[0].present = true" not in detect

start_start = SOURCE.index("bool CirrusAudioFixup::start(")
start_end = SOURCE.index("\n}", start_start)
start = SOURCE[start_start:start_end]
assert start.index("resolvePlatformProfile();") < start.index("performPlatformHardwareReset();")
assert start.index("performPlatformHardwareReset();") < start.index("detectAmplifiers();")

print("PASS device registry, platform profiles, fail-closed discovery and reset ordering")
