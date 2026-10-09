from pathlib import Path
import importlib.util
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_metadata", ROOT / "Tools/release_metadata.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
info = {"CFBundleVersion": "1.0.1", "CFBundleShortVersionString": "1.0.1"}
notes = "## [1.0.1] - 2026-10-10\n\nFixes.\n\n## [1.0.0]\nOld notes.\n"
assert module.validate(notes, info, "v1.0.1") == ("1.0.1", "Fixes.")
for text, plist, tag in [
    (notes, info, "v1.0.0"),
    (notes, {**info, "CFBundleVersion": "1.0.0"}, "v1.0.1"),
    ("## [Unreleased]\nPending fixes.\n" + notes, info, "v1.0.1"),
    ("## [Unreleased]\n", info, None),
]:
    try:
        module.validate(text, plist, tag)
    except ValueError:
        pass
    else:
        raise AssertionError("Invalid release identity accepted")
subprocess.run([sys.executable, str(ROOT / "Tools/release_metadata.py")], check=True)
print("PASS tag/version mismatch and pending release rejection")
