from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "Tools/Collectors/collect_macos.sh").read_text()
assert source.count("capture dmesg /sbin/dmesg") == 1
assert source.index("capture dmesg /sbin/dmesg") < source.index("capture system /usr/bin/sw_vers")
start = source.index("capture cirrus-audio-log ")
end = source.index('\n\nif [ "$#"', start)
command = source[start:end]
assert '"$output/dmesg.txt"' in command
git_bash = Path("C:/Program Files/Git/bin/bash.exe")
bash = str(git_bash) if git_bash.is_file() else shutil.which("bash")
assert bash
with tempfile.TemporaryDirectory(prefix="cirrus-log-sources-") as directory:
    path = Path(directory)
    (path / "kernel-log.txt").write_text("unrelated unified log\n")
    (path / "dmesg.txt").write_text("CirrusAudioFixup: ERROR: retained kernel failure\n")
    script = 'output=.\nkernel_log=./kernel-log.txt\ncapture(){ shift; "$@"; }\n' + command
    result = subprocess.run([bash], input=(script + chr(10)).encode(), cwd=path, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert b"retained kernel failure" in result.stdout
print("PASS collector preserves Cirrus messages present only in dmesg")
