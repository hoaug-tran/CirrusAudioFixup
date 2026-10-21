"""Run the real PowerShell collector helpers on fixture-only files and devices."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import shutil
import subprocess
import os

root = Path(__file__).resolve().parents[2]
shell = shutil.which("powershell.exe") or shutil.which("pwsh")
if shell:
    environment = os.environ.copy()
    # A parent pwsh session can pass a PS7-only module path to Windows PS5.1.
    environment.pop("PSModulePath", None)
    subprocess.run([shell, "-NoProfile", "-NonInteractive", "-File", str(root / "Tests/Build/check_windows_collector.ps1")], check=True, env=environment)
else:
    print("SKIP Windows collector execution: PowerShell unavailable")
