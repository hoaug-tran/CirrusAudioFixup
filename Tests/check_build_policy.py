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
bool gCirrusDebug = cirrus::support::kDefaultVerboseLogging;
int main() {
    IOLog("build=%s\n", cirrus::support::kBuildConfiguration);
    CIRRUS_LOG("default");
    CIRRUS_ERR("essential");
    gCirrusDebug = true;
    CIRRUS_LOG("requested");
    gCirrusDebug = false;
    CIRRUS_LOG("disabled");
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
assert 'switch (amp.model)' in driver and 'initializeCS35L41(amp);' in driver

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
assert 'Playback::advance' in monitor and 'Playback::commit' in monitor

for file in (ROOT / "CirrusAudioFixup").rglob("*"):
    if file.suffix not in (".hpp", ".cpp"):
        continue
    content = file.read_text(encoding="utf-8")
    assert content.startswith("//\n") and any(file.name in line for line in content.splitlines()[:8]), file
    for line in content.splitlines():
        assert not re.match(r"^\s*[^/\s].*\s+//", line), f"Trailing comment in {file}: {line}"

print("PASS build policy: deployment/SDK, logging, symbols, dispatch, sleeping polls, boot-arg docs and comment style")
