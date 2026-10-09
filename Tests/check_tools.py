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
