"""Check firmware CLI failures and collector structure without claiming macOS runtime coverage."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
tool = ROOT / "Tools/import_firmware.py"
with tempfile.TemporaryDirectory(prefix="cirrus-tools-") as directory:
    tmp = Path(directory)
    firmware = tmp / "cs35l41.wmfw"
    left = tmp / "left.bin"
    right = tmp / "right.bin"
    firmware.write_bytes(b"WMFW" + struct.pack("<IHBB", 40, 0, 4, 2) + bytes(28))
    left.write_bytes(b"WMDR" + bytes(12))
    right.write_bytes(left.read_bytes())
    command = [sys.executable, str(tool), "--codec", "cs35l41", "--ssid", "0x17aa3847",
               "--wmfw", str(firmware), "--bin-l", str(left)]
    def run(arguments, expected):
        result = subprocess.run(command + arguments, capture_output=True, text=True)
        assert (result.returncode == 0) == expected, result.stdout + result.stderr
    run(["--validate-only"], True)
    run(["--bin-r", str(right), "--validate-only"], True)
    right.write_bytes(b"invalid")
    run(["--bin-r", str(right), "--validate-only"], False)
    missing = tmp / "missing.bin"
    run(["--bin-r", str(missing), "--validate-only"], False)
    # Write mode must reject the missing channel before it can touch the header.
    header = tmp / "Firmware.hpp"
    header.write_text("unchanged", encoding="utf-8")
    run(["--bin-r", str(missing), "--firmware", str(header)], False)
    assert header.read_text(encoding="utf-8") == "unchanged"
    gain_left = tmp / "left.bincfg"
    gain_right = tmp / "right.bincfg"
    def gain_data(code):
        return struct.pack('<8I', 0x109A4A35, 1, 32, 1, 0, 0, 16, code)
    gain_left.write_bytes(gain_data(14))
    gain_right.write_bytes(gain_data(16))
    run(["--bincfg-l", str(gain_left), "--bincfg-r", str(gain_right), "--validate-only"], True)
    run(["--bincfg-r", str(missing), "--validate-only"], False)
    gain_right.write_bytes(gain_data(21))
    run(["--bincfg-r", str(gain_right), "--validate-only"], False)
    gain_right.write_bytes(gain_data(16))
    header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--bincfg-l", str(gain_left), "--bincfg-r", str(gain_right), "--firmware", str(header)], True)
    imported = header.read_text(encoding='utf-8')
    assert 'SHA256:' in imported and 'l0_bincfg' in imported and 'r0_bincfg' in imported
    def compile_profile(expected_right):
        declarations = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')
        resource = declarations[declarations.index('struct FirmwareResource {'):declarations.index('class CirrusAudioFixup :')]
        cpp = tmp / 'profile.cpp'
        cpp.write_text('#include <cassert>\n#include <cstdint>\n#include <cstddef>\n'
                       '#include "Devices/CS35L41/Resources/Tuning.hpp"\n' + resource + imported + f'''
int main() {{
    const auto& resource=cs35l41Firmware[0];
    cirrus::devices::cs35l41::tuning::Parameters left, right;
    assert(cirrus::devices::cs35l41::tuning::parse(resource.tuningLeft,resource.tuningLeftSize,left));
    assert(cirrus::devices::cs35l41::tuning::parse(resource.tuningRight,resource.tuningRightSize,right));
    assert(left.pcmGain==14 && left.overridden);
    assert(right.pcmGain=={expected_right});
    assert(right.overridden=={str(expected_right != 17).lower()});
}}
''', encoding='utf-8')
        exe = tmp / 'profile.exe'
        subprocess.run(['g++','-std=c++17','-I',str(ROOT / 'CirrusAudioFixup'),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    compile_profile(16)
    run(["--bincfg-l", str(gain_left), "--firmware", str(header)], False)
    assert header.read_text(encoding='utf-8') == imported
    header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--bincfg-l", str(gain_left), "--firmware", str(header)], True)
    imported = header.read_text(encoding='utf-8')
    assert 'l0_bincfg), nullptr, 0,' in imported and 'r0_bincfg' not in imported
    compile_profile(17)

collector = ROOT / "Tools/collect_macos.sh"
source = collector.read_text(encoding="utf-8")
assert "set -o pipefail" in source and "/bin/bash -o pipefail" in source
assert "header_size" in source and "empty output" in source
assert "-c AppleALC" not in source and "-c CS35L41" not in source
bash = shutil.which("bash")
if sys.platform == "win32":
    git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    bash = str(git_bash) if git_bash.exists() else None
assert bash, "Bash is required for collector syntax validation"
subprocess.run([bash, "-n", collector.as_posix()], check=True)
builder = ROOT / "Tools/build_macos.sh"
subprocess.run([bash, "-n", builder.as_posix()], check=True)
build_source = builder.read_text(encoding="utf-8")
assert 'lipo "$binary" -verify_arch x86_64' in build_source
assert '"$binary_uuid" == "$symbols_uuid"' in build_source
assert "kextload" not in build_source and "kmutil" not in build_source
if sys.platform != "darwin":
    denied = subprocess.run([bash, builder.as_posix()], capture_output=True, text=True)
    assert denied.returncode != 0 and "no bootable artifact produced" in denied.stderr
# Exercise the actual capture helpers in isolation. macOS system commands are
# deliberately not invoked by this host regression.
helpers = source[source.index("capture() {"):source.index("capture system ")]
with tempfile.TemporaryDirectory(prefix="cirrus-capture-") as directory:
    script = "output=.\n" + helpers + """
capture empty true
capture failed /bin/bash -c 'exit 7'
capture_shell failed-pipeline 'false | true'
true
"""
    subprocess.run([bash, "-c", script], cwd=directory, check=True)
    status = (Path(directory) / "status.txt").read_text(encoding="utf-8")
    assert "exit=7" in status and "exit=1" in status and "empty output" in status
assert "kernel-log-fallback" in source
print("PASS: firmware CLI regressions, collector syntax and capture failures (macOS execution not tested)")
