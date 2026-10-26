import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import importlib.util
import subprocess
import shlex
import re
import os
import tempfile
import textwrap
import shutil

ROOT = Path(__file__).resolve().parents[2]
bash = shutil.which('bash')
if sys.platform == 'win32':
    git_bash = Path('C:/Program Files/Git/bin/bash.exe')
    if git_bash.exists():
        bash = str(git_bash)
assert bash, 'bash is required for the release gate fixture'
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
triggers = workflow.split("\njobs:", 1)[0]
assert re.search(r"  push:\n    branches: \['\*\*'\]\n    tags: \['v\*'\]", triggers)
assert re.search(r"  pull_request:\n  workflow_dispatch:", triggers)
build_job, release_job = workflow.split("\n  release:\n", 1)
assert 'needs: [build, windows-collector]' in release_job
for name in ("Record build source identity", "Build Release kext", "Build Debug kext", "Verify build artifacts",
             "Package kext bundles", "Upload Release artifact", "Upload Debug artifact", "Upload matching debug symbols"):
    step = build_job.split("      - name: " + name + "\n", 1)[1].split("\n      - name:", 1)[0]
    conditions = re.findall(r"^        if: (.+)$", step, re.M)
    assert conditions == ["github.event_name != 'pull_request'"], name + " must skip PRs and run on pushes/manual runs"
for name in ("Run host regression test suite", "Test Windows PowerShell 5.1 collector helpers", "Test PowerShell 7 collector helpers"):
    step = build_job.split("      - name: " + name + "\n", 1)[1].split("\n      - name:", 1)[0]
    assert not re.search(r"^        if:", step, re.M), name + " must also run on PRs"
assert "if: github.event_name == 'push' && github.ref == 'refs/heads/main'" in release_job
assert 'fetch-depth: 0' in release_job
gate = release_job.split('      - name: Check release status\n', 1)[1].split('\n      - name:', 1)[0]
gate = textwrap.dedent(gate.split('run: |\n', 1)[1])
assert release_job.index('Check release status') < release_job.index('Publish a new version')
with tempfile.TemporaryDirectory(prefix='cirrus-release-gate-') as directory:
    tmp = Path(directory)
    output_file = tmp / 'output.txt'
    mock_gh = 'gh() { if [ "$3" = "v1.0.0" ]; then echo false; elif [ "$3" = "v1.1.1" ]; then echo true; else return 1; fi; }\n'
    test_env = {**os.environ, 'GITHUB_OUTPUT': str(output_file)}
    for tag_name, expected_skip, expected_draft in (('v1.0.0', True, None), ('v1.1.1', False, 'true'), ('v2.0.0', False, 'not_found')):
        output_file.unlink(missing_ok=True)
        result = subprocess.run([bash, '-eu', '-c', mock_gh + gate], cwd=tmp,
                                env={**test_env, 'TAG': tag_name}, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        content = output_file.read_text(encoding='utf-8')
        assert ('skip=true' in content) == expected_skip
        if expected_draft:
            assert f'is_draft={expected_draft}' in content
assert "xcodebuild -version |" not in workflow, "Do not close Xcode's stdout before it finishes"
lipo_commands = [shlex.split(line.strip()) for line in workflow.splitlines()
                 if line.strip().startswith("lipo ")]
assert lipo_commands == [["lipo", "${BUNDLE}/Contents/MacOS/CirrusAudioFixup",
                          "-verify_arch", "x86_64"]], "lipo input must precede -verify_arch"
spec = importlib.util.spec_from_file_location("release_metadata", ROOT / "Tools/Build/release_metadata.py")
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
    (notes + "\n## [1.0.1]\nDuplicate.\n", info, "v1.0.1"),
    ("## [1.0.1]\n\n", info, "v1.0.1"),
    ("## [1.0.1]\n\n### Fixed\n\n", info, "v1.0.1"),
]:
    try:
        module.validate(text, plist, tag)
    except ValueError:
        pass
    else:
        raise AssertionError("Invalid release identity accepted")
subprocess.run([sys.executable, str(ROOT / "Tools/Build/release_metadata.py")], check=True)
print("PASS tag/version mismatch and pending release rejection")
print("PASS CI lipo verification argument order")
print("PASS PR tests only, push/manual build artifacts, main automated release and complete Xcode version output")
