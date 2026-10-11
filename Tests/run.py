"""Run maintained regressions; generated fixtures stay in temporary directories."""
from pathlib import Path
import argparse
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--audit", action="store_true", help="also test the optional Windows package audit tool")
args = parser.parse_args()
# Explicit membership: a one-off experiment must never enter CI via a glob.
suites = (
    "reproduce_host", "check_registers", "check_transport", "check_diagnostics",
    "check_extensible_core", "check_amd_spi", "check_amd_spi_lifecycle", "check_acpi_spi", "check_spi_transport", "check_intel_spi", "check_intel_spi_lifecycle", "check_hda", "check_calibration", "check_tuning", "check_bringup",
    "check_runtime", "check_audio_events", "check_firmware_identity",
    "check_architecture", "check_release", "check_build_policy", "check_tools",
    "check_vtable_policy", "check_xcode_project",
)
for suite in suites + (("check_windows_payloads",) if args.audit else ()):
    print(f"Running {suite}", flush=True)
    subprocess.run([sys.executable, str(ROOT / "Tests" / f"{suite}.py")], cwd=ROOT, check=True)
print("PASS maintained host regressions (hardware behavior requires on-device validation)")
