"""Check build identity, logging defaults and the declared deployment floor."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
project = (ROOT / "CirrusAudioFixup.xcodeproj/project.pbxproj").read_text(encoding="utf-8")
targets = re.findall(r"MACOSX_DEPLOYMENT_TARGET = ([0-9.]+);", project)
assert targets == ["11.0"] * 4, "Project and target configurations must agree on Big Sur"
assert "GCC_OPTIMIZATION_LEVEL = 0;" in project
assert "GCC_OPTIMIZATION_LEVEL = s;" in project
assert project.count('DEBUG_INFORMATION_FORMAT = "dwarf-with-dsym";') == 2
assert project.count("STRIP_INSTALLED_PRODUCT = NO;") == 2
assert project.count('KERNEL_FRAMEWORK_HEADERS = "$(PROJECT_DIR)/MacKernelSDK/Headers";') == 2
assert project.count('"$(PROJECT_DIR)/MacKernelSDK/Library/x86_64"') == 2

source = r'''
#include "Support/Logging.hpp"
#include <cassert>
bool gCirrusDebug = cirrus::support::kDefaultVerboseLogging;
int main() {
    IOLog("build=%s\n", cirrus::support::kBuildConfiguration);
    CIRRUS_LOG("default");
    CIRRUS_ERR("essential");
    gCirrusDebug = true;
    CIRRUS_LOG("requested");
    gCirrusDebug = false;
    int evaluations=0;
    CIRRUS_LOG("disabled %d", ++evaluations);
    assert(evaluations==0);
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-build-policy-") as directory:
    tmp = Path(directory)
    cpp = tmp / "logging.cpp"
    cpp.write_text(source, encoding="utf-8")
    for mode, flags in (("Debug", ["-DDEBUG=1", "-O0"]), ("Release", ["-Os"]),
                        ("Release", ["-DDEBUG=0", "-Os"])):
        exe = tmp / "logging.exe"
        subprocess.run(["g++", "-std=c++17", *flags, "-I", str(ROOT / "CirrusAudioFixup"),
                        str(cpp), "-o", str(exe)], check=True)
        output = subprocess.check_output([str(exe)], text=True)
        assert f"build={mode}" in output
        assert ("CirrusAudioFixup: default" in output) == (mode == "Debug")
        assert "ERROR: essential" in output and "CirrusAudioFixup: requested" in output
        assert "CirrusAudioFixup: disabled" not in output

driver = (ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text(encoding="utf-8")
assert driver.count('kDefaultVerboseLogging || bootArgEnabled("-cirrusdbg")') == 2
assert 'setProperty("Cirrus_Build_Configuration"' in driver
backend = (ROOT / 'CirrusAudioFixup/Devices/CS35L41/CS35L41Backend.cpp').read_text()
assert 'mAmplifierRuntime.bringUp()' in driver and 'createBackend(' in backend
assert 'switch (amp.model)' not in driver

from production_source_fixture import source as production_source
driver=production_source()
readme = (ROOT / "README.md").read_text(encoding="utf-8")
source_flags = set(re.findall(r'"(-cirrus[a-z0-9]+)"', driver))
documented_flags = set(re.findall(r'^\| `(-cirrus[a-z0-9]+)(?:=[^`]*)?`', readme, re.M))
assert source_flags == documented_flags, f"Boot-argument reference differs: {source_flags ^ documented_flags}"
assert 'os_log' not in readme and '100% Working' not in readme

for name in ("stopPlayback", "pollRegisterBit", "sendMailboxCommand"):
    match = re.search(r'(?:bool|void) CirrusAudioFixup::' + name + r'\(', driver)
    function = driver[match.start():driver.index('\n}', match.start())]
    assert "IODelay(1000)" not in function and "IOSleep(1)" in function, name

monitor_start = driver.index('void CirrusAudioFixup::runBackgroundMonitor()')
monitor = driver[monitor_start:driver.index('\n}', monitor_start)]
assert 'IOSleep(' not in monitor and 'IODelay(' not in monitor
assert 'mAmplifierRuntime.runtimeTick' in monitor and 'mAmplifierRuntime.startPlayback' in monitor
assert 'Playback::advance' not in monitor and 'Playback::commit' not in monitor
assert 'Playback::advance' in driver and 'Playback::commit' in driver

print("PASS build policy: deployment/SDK, logging, symbols, dispatch, sleeping polls, boot-arg docs")
