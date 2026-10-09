from pathlib import Path
import importlib.util
import subprocess
import sys
import shlex

ROOT = Path(__file__).resolve().parents[1]
# Apple's -verify_arch consumes every remaining argument as an architecture.
# The input path must precede that operation, including paths containing spaces.
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
lipo_commands = [shlex.split(line.strip()) for line in workflow.splitlines()
                 if line.strip().startswith("lipo ")]
assert lipo_commands == [["lipo", "${BUNDLE}/Contents/MacOS/CirrusAudioFixup",
                          "-verify_arch", "x86_64"]], "lipo input must precede -verify_arch"
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
print("PASS CI lipo verification argument order")
