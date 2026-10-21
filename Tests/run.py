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
    "Drivers/reproduce_host", "Drivers/check_registers", "Transport/check_transport", "Platform/check_diagnostics", "Platform/check_boot_evidence", "Platform/check_attachment", "Platform/check_board_metadata",
    "Architecture/check_extensible_core", "Drivers/check_cs35l41_driver", "Drivers/check_cs35l41_power", "Platform/check_platform_configuration", "Platform/check_platform_discovery", "Transport/check_native_spi", "Transport/check_spi_chip_select", "Transport/check_spi_gpio", "Transport/check_spi_gpio_binding", "Transport/check_amd_spi", "Transport/check_amd_spi_lifecycle", "Transport/check_acpi_spi", "Transport/check_acpi_cast", "Transport/check_pci_publication", "Transport/check_spi_transport", "Transport/check_intel_spi", "Transport/check_intel_spi_lifecycle", "Transport/check_spi_power_client", "Platform/check_hda", "Drivers/check_calibration", "Drivers/check_tuning", "Platform/check_bringup",
    "Platform/check_runtime", "Platform/check_audio_events", "Drivers/check_firmware_identity",
    "Architecture/check_architecture", "Build/check_release", "Build/check_build_policy", "Build/check_tools",
    "Build/check_vtable_policy", "Build/check_xcode_project", "Architecture/check_source_layout", "Build/check_windows_collector",
)
for suite in suites + (("Build/check_windows_payloads",) if args.audit else ()):
    print(f"Running {suite}", flush=True)
    subprocess.run([sys.executable, str(ROOT / "Tests" / f"{suite}.py")], cwd=ROOT, check=True)
print("PASS maintained host regressions (hardware behavior requires on-device validation)")
