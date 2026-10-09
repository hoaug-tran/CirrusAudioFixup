"""Validate release identity and emit metadata for CI (standard library only)."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re


def validate(changelog, info, tag=None):
    blocks = list(re.finditer(r"^## \[([^\]]+)\].*\n([\s\S]*?)(?=^## \[|\Z)", changelog, re.M))
    releases = [b for b in blocks if b.group(1) != "Unreleased"]
    if not releases:
        raise ValueError("No versioned changelog entry")
    version = releases[0].group(1)
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Release version must be X.Y.Z")
    if any(info.get(k) != version for k in ("CFBundleVersion", "CFBundleShortVersionString")):
        raise ValueError("Changelog and bundle versions differ")
    if tag is not None and tag != "v" + version:
        raise ValueError("Tag and release version differ")
    if tag and any(b.group(1) == "Unreleased" and b.group(2).strip() for b in blocks):
        raise ValueError("Cannot publish with pending Unreleased notes")
    return version, releases[0].group(2).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--bundle", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    info = plistlib.loads((root / "CirrusAudioFixup/Info.plist").read_bytes())
    version, notes = validate((root / "CHANGELOG.md").read_text(encoding="utf-8"), info, args.tag)
    if args.bundle:
        bundled = plistlib.loads((args.bundle / "Contents/Info.plist").read_bytes())
        validate((root / "CHANGELOG.md").read_text(encoding="utf-8"), bundled, args.tag)
        if bundled.get("CFBundleIdentifier") != info.get("CFBundleIdentifier"):
            raise ValueError("Built bundle identifier differs from source")
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        (args.output_dir / "RELEASE_NOTES.md").write_text(notes + "\n", encoding="utf-8")
        (args.output_dir / "BUILD_METADATA.json").write_text(json.dumps({
            "version": version, "tag": args.tag, "commit": os.environ.get("GITHUB_SHA"),
            "run_url": f'https://github.com/{os.environ.get("GITHUB_REPOSITORY", "")}/actions/runs/{os.environ.get("GITHUB_RUN_ID", "")}',
        }, indent=2) + "\n", encoding="utf-8")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"version={version}\ntag=v{version}\n")
    print(f"PASS release identity: {version}")


if __name__ == "__main__":
    main()
