#!/usr/bin/env python3
"""Evidence-driven onboarding helper for Level 2 CS35L41/CS35L56 hardware candidates."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import urllib.request
from pathlib import Path

WORKTREE_ROOT = Path(__file__).resolve().parents[2]
CATALOG_JSON = WORKTREE_ROOT / "docs/hardware/hardware_catalog.json"
BOARD_PROPS_HPP = (
    WORKTREE_ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Board/BoardProperties.hpp"
)
CS35L56_DISCOVERY_HPP = (
    WORKTREE_ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
)

sys.path.insert(0, str(WORKTREE_ROOT / "Tools/Firmware"))
from import_firmware import validate_bin, validate_wmfw  # noqa: E402

LINUX_FIRMWARE_RAW_BASE = (
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/"
)


def normalize_ssid(raw: str) -> str:
    cleaned = raw.strip().lower().removeprefix("0x").replace(":", "")
    if not re.fullmatch(r"[0-9a-f]{8}", cleaned):
        raise ValueError(f"Invalid subsystem ID {raw!r}; expected XXXX:YYYY or XXXXXXXX")
    return f"{cleaned[:4].upper()}:{cleaned[4:].upper()}"


def load_catalog() -> tuple[dict, dict[str, dict], dict[str, str]]:
    catalog = json.loads(CATALOG_JSON.read_text(encoding="utf-8"))
    by_sub = {e["subsystem"].upper(): e for e in catalog["entries"]}
    aliases = {a["from"].upper(): a["to"].upper() for a in catalog.get("aliases", [])}
    return catalog, by_sub, aliases


def parse_l41_board_props() -> dict[str, dict]:
    text = BOARD_PROPS_HPP.read_text(encoding="utf-8")
    out: dict[str, dict] = {}
    for m in re.finditer(
        r"\{0x([0-9a-fA-F]{8}),\s*(\d+),\s*(true|false),\s*\{([^}]+)\},\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}",
        text,
    ):
        raw = m.group(1).upper()
        sub = f"{raw[:4]}:{raw[4:]}"
        out[sub] = {
            "count": int(m.group(2)),
            "boost_type": "internal" if m.group(3) == "true" else "external",
            "channels": [int(x.strip()) for x in m.group(4).split(",")],
            "reset_resource": int(m.group(5)),
            "speaker_resource": int(m.group(6)),
            "cs_resource": int(m.group(7)),
            "inductor_nh": int(m.group(8)),
            "peak_ma": int(m.group(9)),
            "capacitor_uf": int(m.group(10)),
        }
    return out


def parse_l56_board_props() -> dict[str, dict]:
    text = CS35L56_DISCOVERY_HPP.read_text(encoding="utf-8")
    out: dict[str, dict] = {}
    for m in re.finditer(
        r"\{0x([0-9a-fA-F]{8}),\s*core::BusTransportType::(I2C|SPI),\s*(\d+),\s*\{([^}]+)\},\s*\{([^}]+)\},\s*(\d+),\s*(-?\d+)\}",
        text,
    ):
        raw = m.group(1).upper()
        sub = f"{raw[:4]}:{raw[4:]}"
        out[sub] = {
            "transport": m.group(2),
            "count": int(m.group(3)),
            "addresses": [int(x.strip(), 0) for x in m.group(4).split(",")],
            "channels": [int(x.strip()) for x in m.group(5).split(",")],
            "speaker_id": int(m.group(6)),
            "reset_resource": int(m.group(7)),
        }
    return out


def resolve_entry(raw_ssid: str) -> tuple[dict, str | None, dict | None]:
    sub = normalize_ssid(raw_ssid)
    _, by_sub, aliases = load_catalog()
    alias_from = None
    if sub not in by_sub and sub in aliases:
        alias_from = sub
        sub = aliases[sub]
    if sub not in by_sub:
        raise KeyError(f"Subsystem ID {sub} is not present in hardware_catalog.json")
    entry = by_sub[sub]
    props = (
        parse_l41_board_props().get(sub)
        if entry["family"] == "CS35L41"
        else parse_l56_board_props().get(sub)
    )
    return entry, alias_from, props


def build_import_commands(entry: dict) -> list[str]:
    files = entry.get("upstream_firmware_files", [])
    if not files:
        return []
    ssid_hex = entry["subsystem_hex"].removeprefix("0x").lower()
    wmfw_list = [f for f in files if f.lower().endswith(".wmfw")]
    if not wmfw_list:
        return []
    wmfw = Path(wmfw_list[0]).name
    bins = [f for f in files if f.lower().endswith(".bin")]
    cmds: list[str] = []
    if entry["family"] == "CS35L41":
        groups: dict[int | None, dict[str, str]] = {}
        for p in bins:
            name = Path(p).name
            low = name.lower()
            sm = re.search(r"-spkid(\d+)", low)
            sk = int(sm.group(1)) if sm else None
            grp = groups.setdefault(sk, {})
            if low.endswith("-l0.bin") or low.endswith("-l.bin"):
                grp["l"] = name
            elif low.endswith("-r0.bin") or low.endswith("-r.bin"):
                grp["r"] = name
        for sk in sorted(groups, key=lambda k: (-1 if k is None else k)):
            grp = groups[sk]
            if "l" in grp and "r" in grp:
                spk_flag = "--generic-speaker" if sk is None else f"--spkid {sk}"
                cmds.append(
                    f"python3 Tools/Firmware/import_firmware.py --ssid {ssid_hex} --codec cs35l41 "
                    f"{spk_flag} --wmfw firmware/{wmfw} --bin-l firmware/{grp['l']} --bin-r firmware/{grp['r']}"
                )
    else:
        amp1 = next((Path(p).name for p in bins if "amp1" in p.lower()), None)
        amp2 = next((Path(p).name for p in bins if "amp2" in p.lower()), None)
        if amp1 and amp2:
            cmds.append(
                f"python3 Tools/Firmware/import_firmware.py --ssid {ssid_hex} --codec cs35l56 "
                f"--spkid 0 --wmfw firmware/{wmfw} --bin-l firmware/{amp1} --bin-r firmware/{amp2}"
            )
    return cmds


def physical_checklist(entry: dict) -> list[str]:
    sub = entry["subsystem"]
    return [
        f"Verify OpenCore ACPI/PCI identity reports {sub} and matched HID ({entry['family']}).",
        "Cold boot macOS and verify Cirrus_Driver_Verdict = READY_DSP, Cirrus_Detected_Amplifiers = 2, and zero Cirrus_Diag_FirstFailure_*.",
        "Verify Audio MIDI Setup sample rate (48000 Hz) and distinct L/R speaker output.",
        "Stop playback and verify Cirrus_Playback_Verdict_left/_right transition cleanly to SAFE_IDLE_VERIFIED.",
        "Test mute/unmute, 3.5 mm jack plug/unplug (3+ cycles), and >= 3 S3 sleep/wake cycles.",
        "Archive evidence with `sudo bash Tools/Collectors/collect_macos.sh` before promoting board to README.md (Level 3).",
    ]


def cmd_list_ready() -> int:
    catalog, _, _ = load_catalog()
    ready = [e for e in catalog["entries"] if e["level"] == 2]
    print(f"Technically supportable (Level 2) boards: {len(ready)}")
    for e in ready:
        n_fw = len(e.get("upstream_firmware_files", []))
        print(
            f"  {e['subsystem']} | {e['oem']:6s} | {e['family']} | {e['bus_hint']:3s} | "
            f"fw={n_fw:2d} | {e['model']}"
        )
    return 0


def cmd_inspect(raw_ssid: str) -> int:
    try:
        entry, alias_from, props = resolve_entry(raw_ssid)
    except (ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"Subsystem : {entry['subsystem']} ({entry['subsystem_hex']})")
    if alias_from:
        print(f"Alias From: {alias_from} -> {entry['subsystem']}")
    print(f"Model     : {entry['oem']} / {entry['model']}")
    print(f"Family/Bus: {entry['family']} / {entry['bus_hint']} ({entry['amp_count']} amps)")
    print(f"Status    : L{entry['level']} {entry['status']} (missing={entry['missing_reason']})")
    print(f"Provenance: {', '.join(entry['provenance'])}")
    print(f"BoardProps: {json.dumps(props) if props else 'NONE'}")
    urls = [LINUX_FIRMWARE_RAW_BASE + p for p in entry.get("upstream_firmware_files", [])]
    if urls:
        print("Upstream firmware URLs:")
        for u in urls:
            print(f"  - {u}")
    cmds = build_import_commands(entry)
    if cmds:
        print("Firmware import commands (after fetching):")
        for c in cmds:
            print(f"  {c}")
    return 0


def cmd_stage(raw_ssid: str, out_dir: Path, fetch_fw: bool) -> int:
    try:
        entry, alias_from, props = resolve_entry(raw_ssid)
    except (ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if entry["level"] != 2:
        print(
            f"REFUSED: {entry['subsystem']} is L{entry['level']} ({entry['status']}, "
            f"reason={entry['missing_reason']}); only Level 2 TECHNICALLY_SUPPORTABLE candidates may be staged.",
            file=sys.stderr,
        )
        return 2

    if not props:
        print(f"REFUSED: Missing parsed BoardProperties for {entry['subsystem']}", file=sys.stderr)
        return 2

    fw_paths = entry.get("upstream_firmware_files", [])
    urls = [LINUX_FIRMWARE_RAW_BASE + p for p in fw_paths]
    cmds = build_import_commands(entry)
    assert cmds, f"No import_firmware commands generated for {entry['subsystem']}"

    out_dir.mkdir(parents=True, exist_ok=True)
    downloaded: list[dict] = []
    if fetch_fw:
        fw_dir = out_dir / "firmware"
        fw_dir.mkdir(parents=True, exist_ok=True)
        for rel in fw_paths:
            url = LINUX_FIRMWARE_RAW_BASE + rel
            req = urllib.request.Request(url, headers={"User-Agent": "CirrusAudioFixup-Onboard/1.0"})
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()
            name = Path(rel).name
            low = name.lower()
            if low.endswith(".wmfw"):
                ok, msg = validate_wmfw(data)
            elif low.endswith(".bin"):
                ok, msg = validate_bin(data)
            else:
                ok, msg = False, f"Unexpected firmware file extension: {name}"
            if not ok:
                print(f"ERROR validating {rel}: {msg}", file=sys.stderr)
                return 3
            dest = fw_dir / name
            dest.write_bytes(data)
            downloaded.append(
                {
                    "upstream_path": rel,
                    "local_file": str(Path("firmware") / name),
                    "size_bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "validation": msg,
                }
            )

    manifest = {
        "schema_version": 1,
        "authorized": False,
        "alias_resolved_from": alias_from,
        "entry": entry,
        "board_properties": props,
        "upstream_firmware_urls": urls,
        "downloaded_firmware": downloaded,
        "import_commands": cmds,
        "readme_row_template": (
            f"| {entry['model']} | `{entry['subsystem']}` | {entry['family']} | "
            f"{entry['bus_hint']} | ALCxxx | Testing (staged; awaiting physical validation) |"
        ),
        "physical_validation_checklist": physical_checklist(entry),
    }
    manifest_path = out_dir / "staging_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Staged Level 2 candidate {entry['subsystem']} ({entry['model']}) -> {manifest_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list-ready", action="store_true", help="List all Level 2 boards")
    group.add_argument("--inspect", metavar="SSID", help="Inspect onboarding readiness for SSID")
    group.add_argument("--stage", metavar="SSID", help="Stage a Level 2 board bundle")
    parser.add_argument("--out-dir", type=Path, help="Output directory for --stage")
    parser.add_argument(
        "--fetch-firmware",
        action="store_true",
        help="Download and validate upstream WMFW/WMDR binaries into --out-dir/firmware",
    )
    args = parser.parse_args()
    if args.list_ready:
        return cmd_list_ready()
    if args.inspect:
        return cmd_inspect(args.inspect)
    if args.stage:
        if not args.out_dir:
            parser.error("--stage requires --out-dir")
        return cmd_stage(args.stage, args.out_dir, args.fetch_firmware)
    return 1


if __name__ == "__main__":
    sys.exit(main())
