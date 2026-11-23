#!/usr/bin/env python3
"""Synchronize and deterministically verify CirrusAudioFixup's proactive hardware catalog.

Reads authoritative local source-of-truth tables and (with --fetch) upstream Linux HDA
side-codec / Realtek quirk tables and linux-firmware WHENCE, generating:
  - docs/hardware/hardware_catalog.json
  - CirrusAudioFixup/Platform/Board/HardwareCatalog.hpp

Strictly enforces the three-level safety boundary:
  Level 3 (ProductionSupported): Only boards authorized in CirrusAudioFixup's own
    verified runtime/firmware tables (never auto-promoted from upstream Linux).
  Level 2 (TechnicallySupportable): Known 2-amp CS35L41/CS35L56 boards with
    verified upstream firmware/L+R tuning and board metadata (not yet authorized or
    lacking embedded kext firmware).
  Level 1 (RecognizedCandidate): Known upstream boards with missing metadata,
    missing/mismatched tuning, unsupported 4-amp/1-amp topology, or unsupported HID.
"""

from __future__ import annotations

import argparse
import base64
import json
import posixpath
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

WORKTREE_ROOT = Path(__file__).resolve().parents[2]
CATALOG_JSON = WORKTREE_ROOT / "docs/hardware/hardware_catalog.json"
CATALOG_HPP = WORKTREE_ROOT / "CirrusAudioFixup/Platform/Board/HardwareCatalog.hpp"
BOARD_PROPS_HPP = (
    WORKTREE_ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Board/BoardProperties.hpp"
)
CS35L41_FW_INC = (
    WORKTREE_ROOT
    / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/Generated/FirmwareData.inc"
)
CS35L41_FW_CPP = (
    WORKTREE_ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/CS35L41Firmware.cpp"
)
CS35L56_DISCOVERY_HPP = (
    WORKTREE_ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
)
CS35L56_FW_INC = (
    WORKTREE_ROOT
    / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/Resources/Generated/FirmwareData.inc"
)
PLATFORM_PROFILE_HPP = WORKTREE_ROOT / "CirrusAudioFixup/Platform/Board/PlatformProfile.hpp"
README_MD = WORKTREE_ROOT / "README.md"

OEM_NAMES = {
    0x1025: "Acer",
    0x1028: "Dell",
    0x103C: "HP",
    0x1043: "ASUS",
    0x17AA: "Lenovo",
    0x1F66: "AYANEO",
}


def gh_api_json(endpoint: str) -> object:
    out = subprocess.check_output(["gh", "api", endpoint], text=True)
    return json.loads(out)


def gh_raw(path: str, ref: str = "master") -> str:
    obj = gh_api_json(f"repos/torvalds/linux/contents/{path}?ref={ref}")
    return base64.b64decode(obj["content"]).decode("utf-8", errors="replace")


def fetch_url(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "CirrusAudioFixup-CatalogSync/1.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read().decode("utf-8", errors="replace")


def parse_local_production_sources() -> dict:
    def parse_fw_inc(path: Path) -> set[int]:
        text = path.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"const FirmwareResource\s+\w+\[\]\s*=\s*\{(.*?)\};", text, re.S)
        assert m, f"Missing FirmwareResource table in {path}"
        ssids = set()
        for entry in re.findall(r"\{([^{}]+)\}", m.group(1)):
            parts = [p.strip() for p in " ".join(entry.split()).split(",")]
            is_dummy = len(parts) > 11 and parts[11] == "true"
            if not is_dummy:
                ssids.add((int(parts[0], 16) << 16) | int(parts[1], 16))
        return ssids

    l41_fw = parse_fw_inc(CS35L41_FW_INC)
    l56_fw = parse_fw_inc(CS35L56_FW_INC)

    profile_text = PLATFORM_PROFILE_HPP.read_text(encoding="utf-8")
    clsa0100 = {
        int(raw, 16)
        for raw in re.findall(r"pciSubsystemId\s*==\s*0x([0-9a-fA-F]{8})", profile_text)
    }

    fw_cpp = CS35L41_FW_CPP.read_text(encoding="utf-8")
    quirk_block = re.search(r"kSsidQuirks\[\]\s*=\s*\{(.*?)\};", fw_cpp, re.S)
    assert quirk_block
    l41_aliases: dict[int, int] = {}
    for fv, fd, tv, td in re.findall(
        r"\{0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+)\}",
        quirk_block.group(1),
    ):
        l41_aliases[(int(fv, 16) << 16) | int(fd, 16)] = (int(tv, 16) << 16) | int(td, 16)

    readme = README_MD.read_text(encoding="utf-8")
    sec_match = re.search(r"### Supported laptops\s*(.*?)\n### ", readme, re.S)
    assert sec_match
    readme_models: dict[int, dict] = {}
    for line in sec_match.group(1).splitlines():
        m = re.match(
            r"^\|\s*([^|]+?)\s*\|\s*`([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})`\s*\|\s*(CS35L41|CS35L56)\s*\|\s*(I2C|SPI)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|",
            line.strip(),
        )
        if m:
            ssid = (int(m.group(2), 16) << 16) | int(m.group(3), 16)
            readme_models[ssid] = {
                "model": m.group(1).strip(),
                "family": m.group(4),
                "bus": m.group(5),
                "codec": m.group(6).strip(),
            }

    board_props_text = BOARD_PROPS_HPP.read_text(encoding="utf-8")
    l41_board_props: dict[int, dict] = {}
    for m in re.finditer(
        r"\{0x([0-9a-fA-F]{8}),\s*(\d+),\s*(true|false),\s*\{([^}]+)\},\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}",
        board_props_text,
    ):
        ssid = int(m.group(1), 16)
        l41_board_props[ssid] = {
            "count": int(m.group(2)),
            "internal_boost": m.group(3) == "true",
            "channels": [int(x.strip()) for x in m.group(4).split(",")],
            "reset_resource": int(m.group(5)),
            "speaker_resource": int(m.group(6)),
            "cs_resource": int(m.group(7)),
            "inductor_nh": int(m.group(8)),
            "peak_ma": int(m.group(9)),
            "capacitor_uf": int(m.group(10)),
        }

    l56_text = CS35L56_DISCOVERY_HPP.read_text(encoding="utf-8")
    l56_board_props: dict[int, dict] = {}
    for m in re.finditer(
        r"\{0x([0-9a-fA-F]{8}),\s*core::BusTransportType::(I2C|SPI),\s*(\d+),\s*\{([^}]+)\},\s*\{([^}]+)\},\s*(\d+),\s*(-?\d+)\}",
        l56_text,
    ):
        ssid = int(m.group(1), 16)
        l56_board_props[ssid] = {
            "bus": m.group(2),
            "count": int(m.group(3)),
            "addresses": [int(x.strip(), 0) for x in m.group(4).split(",")],
            "channels": [int(x.strip()) for x in m.group(5).split(",")],
            "speaker_id": int(m.group(6)),
            "reset_resource": int(m.group(7)),
        }

    return {
        "l41_fw": l41_fw,
        "l56_fw": l56_fw,
        "clsa0100": clsa0100,
        "l41_aliases": l41_aliases,
        "readme_models": readme_models,
        "l41_board_props": l41_board_props,
        "l56_board_props": l56_board_props,
    }


def parse_whence(whence: str) -> tuple[dict[int, dict], dict[int, dict], dict[str, str]]:
    from collections import defaultdict

    real_files: set[str] = set()
    link_targets: dict[str, str] = {}
    for line in whence.splitlines():
        if line.startswith("File:"):
            real_files.add(line.split()[1])
        elif line.startswith("Link:"):
            m_link = re.match(r"Link:\s*(\S+)\s*->\s*(\S+)", line)
            if m_link:
                src, dst = m_link.group(1), m_link.group(2)
                link_targets[src] = posixpath.normpath(posixpath.join(posixpath.dirname(src), dst))

    resolved_map: dict[str, str] = {}
    for p in real_files:
        resolved_map[p] = p
    for src in link_targets:
        cur = src
        for _ in range(10):
            if cur in real_files:
                resolved_map[src] = cur
                break
            if cur not in link_targets:
                break
            cur = link_targets[cur]

    def empty_l41():
        return {
            "wmfw": set(),
            "bins": set(),
            "l1_r1": set(),
            "spkids": defaultdict(lambda: {"l0": set(), "r0": set()}),
            "cali": set(),
            "raw": set(),
        }

    l41 = defaultdict(empty_l41)
    l56 = defaultdict(lambda: {"wmfw": set(), "amp1": set(), "amp2": set(), "amp34": set(), "amplr": set(), "raw": set()})

    # Cross-check WHENCE entries while strictly separating spk-prot playback assets from calibration-only blobs.
    for line in whence.splitlines():
        if not (line.startswith("File:") or line.startswith("Link:")):
            continue
        path = line.split()[1]
        if not (path.startswith("cirrus/cs35l41") or path.startswith("cirrus/cs35l56")):
            continue
        if path not in resolved_map:
            continue
        m = re.search(r"-([0-9a-f]{8})(?:-|\.)", path.lower())
        if not m:
            continue
        ssid = int(m.group(1), 16)
        low = path.lower()
        if path.startswith("cirrus/cs35l41"):
            d = l41[ssid]
            d["raw"].add(path)
            if "spk-cali" in low:
                d["cali"].add(path)
                continue
            if "spk-prot" not in low:
                continue
            if low.endswith(".wmfw"):
                d["wmfw"].add(path)
            elif low.endswith((".bin", ".bincfg")):
                d["bins"].add(path)
                if low.endswith("-l1.bin") or low.endswith("-r1.bin"):
                    d["l1_r1"].add(path)
                else:
                    sm = re.search(r"-spkid(\d+)", low)
                    sk = int(sm.group(1)) if sm else -1
                    if low.endswith("-l0.bin") or low.endswith("-l.bin"):
                        d["spkids"][sk]["l0"].add(path)
                    elif low.endswith("-r0.bin") or low.endswith("-r.bin"):
                        d["spkids"][sk]["r0"].add(path)
        else:
            d = l56[ssid]
            d["raw"].add(path)
            if low.endswith(".wmfw"):
                d["wmfw"].add(path)
            elif low.endswith((".bin", ".bincfg")):
                if "amp3" in low or "amp4" in low or re.search(r"-l\d+u\d+", low):
                    if "amp3" in low or "amp4" in low:
                        d["amp34"].add(path)
                    else:
                        d["amplr"].add(path)
                elif "amp1" in low:
                    d["amp1"].add(path)
                elif "amp2" in low:
                    d["amp2"].add(path)
                elif "-ampl" in low or "-ampr" in low:
                    d["amplr"].add(path)
    return l41, l56, resolved_map


def parse_alc269(alc269_c: str) -> dict[int, dict]:
    out = {}
    for m in re.finditer(
        r'SND_PCI_QUIRK\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*"([^"]+)",\s*([^)]+)\)',
        alc269_c,
    ):
        fixup = m.group(4).strip()
        name = m.group(3).strip()
        if not ("cs35l" in fixup.lower() or "csc35" in fixup.lower() or "cs35l" in name.lower()):
            continue
        ssid = (int(m.group(1), 16) << 16) | int(m.group(2), 16)
        bus = "SPI" if "_SPI" in fixup else "I2C" if "_I2C" in fixup else "UNKNOWN"
        amp_count = 4 if ("_4" in fixup) else 1 if ("_1" in fixup) else 2
        out[ssid] = {"model": name, "fixup": fixup, "bus": bus, "amp_count": amp_count}
    return out


def fetch_and_generate() -> None:
    master = gh_api_json("repos/torvalds/linux/commits/master")
    linux_sha = master["sha"]
    linux_date = master["commit"]["committer"]["date"]

    prop_c = gh_raw("sound/hda/codecs/side-codecs/cs35l41_hda_property.c", linux_sha)
    cs56_c = gh_raw("sound/hda/codecs/side-codecs/cs35l56_hda.c", linux_sha)
    alc269_c = gh_raw("sound/hda/codecs/realtek/alc269.c", linux_sha)
    whence = fetch_url("https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/WHENCE")

    row_re = re.compile(
        r'\{\s*"([0-9a-fA-F]{8})",\s*(\d+),\s*(INTERNAL|EXTERNAL),\s*\{\s*([^}]+?)\s*\},\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}'
    )
    sym = {"CS35L41_LEFT": 0, "CS35L41_RIGHT": 1, "0": 0, "1": 1}
    up_l41_props: dict[int, dict] = {}
    for ssid_raw, n_amps, boost, ch_s, rst, spk, cs, ind, pk, cap in row_re.findall(prop_c):
        ssid = int(ssid_raw, 16)
        ch = [sym[x.strip()] for x in ch_s.split(",")]
        while len(ch) < 4:
            ch.append(0)
        up_l41_props[ssid] = {
            "count": int(n_amps),
            "internal_boost": boost == "INTERNAL",
            "channels": ch[:4],
            "reset_resource": int(rst),
            "speaker_resource": int(spk),
            "cs_resource": int(cs),
            "inductor_nh": int(ind),
            "peak_ma": int(pk),
            "capacitor_uf": int(cap),
        }

    # Sync BoardProperties.hpp deterministically with upstream (preserving exact known overrides on 103C89C6 / 103C8CDD / 103C8CDE?
    # Wait: check 103C89C6 carefully! Existing tests check 103C89C6 has reset=1, cs=0 in check_board_metadata.py lines 236-247!
    # We MUST keep existing validated entries intact and only APPEND/merge new upstream rows so zero existing behavior breaks!)
    local = parse_local_production_sources()
    existing_l41_props = local["l41_board_props"]
    merged_l41_props = dict(existing_l41_props)
    added_props = 0
    for ssid, row in sorted(up_l41_props.items()):
        if ssid not in merged_l41_props:
            merged_l41_props[ssid] = row
            added_props += 1

    if added_props > 0:
        write_board_properties_hpp(merged_l41_props)
        print(f"Updated BoardProperties.hpp with {added_props} new upstream Linux rows (total {len(merged_l41_props)})")

    l41_fw_up, l56_fw_up, resolved_whence = parse_whence(whence)
    alc269_quirks = parse_alc269(alc269_c)
    up_l56_fixups = {int(s, 16) for s in re.findall(r'\.sub\s*=\s*"([0-9a-fA-F]{8})"', cs56_c)}

    local = parse_local_production_sources()
    prod_ssids = set(local["readme_models"].keys())
    assert len(prod_ssids) == 14, f"Expected 14 production boards, got {len(prod_ssids)}"

    all_ssids = (
        prod_ssids
        | set(merged_l41_props.keys())
        | set(local["l56_board_props"].keys())
        | set(alc269_quirks.keys())
        | set(l41_fw_up.keys())
        | set(l56_fw_up.keys())
        | up_l56_fixups
    )

    entries = []
    for ssid in sorted(all_ssids):
        vendor = ssid >> 16
        device = ssid & 0xFFFF
        oem = OEM_NAMES.get(vendor, f"0x{vendor:04X}")

        in_prod = ssid in prod_ssids
        in_l41_board = ssid in merged_l41_props
        in_l56_board = ssid in local["l56_board_props"]
        in_alc = alc269_quirks.get(ssid)
        fw41 = l41_fw_up.get(ssid)
        fw56 = l56_fw_up.get(ssid)

        # Determine family carefully without mixing L41/L56
        if in_prod:
            family = local["readme_models"][ssid]["family"]
        elif in_l56_board or ssid in up_l56_fixups or (fw56 and not (in_l41_board or in_alc or fw41)):
            family = "CS35L56"
        else:
            family = "CS35L41"

        # Amp count & bus & model name
        amp_count = 0
        bus = "UNKNOWN"
        model = in_alc["model"] if in_alc else ""
        if in_prod:
            amp_count = 2
            bus = local["readme_models"][ssid]["bus"]
            model = local["readme_models"][ssid]["model"]
        elif family == "CS35L41":
            if in_l41_board:
                amp_count = merged_l41_props[ssid]["count"]
                bus = "SPI" if merged_l41_props[ssid]["cs_resource"] >= 0 else (in_alc["bus"] if in_alc else "I2C_OR_SPI")
            elif in_alc:
                amp_count = in_alc["amp_count"]
                bus = in_alc["bus"]
            elif fw41:
                valid_lr = any(pair["l0"] and pair["r0"] for pair in fw41["spkids"].values())
                amp_count = 4 if fw41["l1_r1"] else 2 if valid_lr else 0
        else:
            if in_l56_board:
                amp_count = local["l56_board_props"][ssid]["count"]
                bus = local["l56_board_props"][ssid]["bus"]
            elif fw56:
                amp_count = 4 if fw56["amp34"] else 2 if ((fw56["amp1"] and fw56["amp2"]) or fw56["amplr"]) else 0

        if not model:
            if ssid == 0x17AA390B:
                model = "Lenovo Yoga Book 9i (Linux cs35l56 quirk)"
            elif in_l41_board:
                model = f"{oem} CSC3551 ({vendor:04X}:{device:04X})"
            else:
                model = f"{oem} {family} ({vendor:04X}:{device:04X})"

        # Determine classification strictly according to audited safety policy
        fw_files: list[str] = []
        if in_prod:
            level = 3
            status = "PRODUCTION_SUPPORTED"
            missing_reason = "NONE"
            fw_available = True
        elif family == "CS35L41":
            valid_pairs = sorted(
                sk for sk, d in (fw41["spkids"].items() if fw41 else ()) if d["l0"] and d["r0"]
            )
            has_distinct_lr = bool(fw41 and fw41["wmfw"] and valid_pairs and not fw41["l1_r1"])
            fw_available = has_distinct_lr
            if amp_count == 4:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNSUPPORTED_QUAD_AMP"
            elif amp_count == 1:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNSUPPORTED_SINGLE_AMP"
            elif not in_l41_board and in_alc:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "REQUIRES_NATIVE_ACPI_DSD_OR_METADATA"
            elif not in_l41_board:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "MISSING_BOARD_METADATA"
            elif not fw41 or not fw41["wmfw"] or not fw41["bins"]:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "MISSING_UPSTREAM_FIRMWARE"
            elif not has_distinct_lr:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNVERIFIED_CHANNEL_COEFFICIENTS"
            else:
                row = merged_l41_props[ssid]
                elec_ok = (not row["internal_boost"]) or (
                    row["inductor_nh"] > 0 and row["peak_ma"] > 0 and row["capacitor_uf"] > 0
                )
                needs_spkid = any(sk >= 0 for sk in valid_pairs) and (-1 not in valid_pairs)
                spk_res = row["speaker_resource"]
                if not elec_ok:
                    level = 1
                    status = "RECOGNIZED_CANDIDATE"
                    missing_reason = "MISSING_BOARD_METADATA"
                elif (needs_spkid and spk_res < 0) or (not needs_spkid and spk_res >= 0):
                    level = 1
                    status = "RECOGNIZED_CANDIDATE"
                    missing_reason = "SPEAKER_ID_GPIO_MISMATCH"
                elif bus == "SPI" or row["cs_resource"] >= 0:
                    level = 1
                    status = "RECOGNIZED_CANDIDATE"
                    missing_reason = "SPI_REQUIRES_NATIVE_ACPI_DSD_AND_FW"
                elif bus != "I2C":
                    level = 1
                    status = "RECOGNIZED_CANDIDATE"
                    missing_reason = "AMBIGUOUS_BUS_TOPOLOGY"
                else:
                    level = 2
                    status = "TECHNICALLY_SUPPORTABLE"
                    missing_reason = "MISSING_EMBEDDED_FW_AND_HW_VALIDATION"
                    picked = set(fw41["wmfw"])
                    for sk in valid_pairs:
                        picked.update(fw41["spkids"][sk]["l0"])
                        picked.update(fw41["spkids"][sk]["r0"])
                    fw_files = sorted({resolved_whence[p] for p in picked})
        else:
            # CS35L56: check B0 vs B2 and amp1/amp2
            has_b0_2amp_fw = bool(
                fw56
                and any("cs35l56-b0" in p.lower() for p in fw56["raw"])
                and ((fw56["amp1"] and fw56["amp2"]) or fw56["amplr"])
                and not fw56["amp34"]
            )
            has_b2_only = bool(fw56 and not any("cs35l56-b0" in p.lower() for p in fw56["raw"]))
            fw_available = has_b0_2amp_fw
            if amp_count == 4:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNSUPPORTED_QUAD_AMP"
            elif has_b2_only:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNSUPPORTED_SILICON_REV_B2"
            elif in_l56_board and fw56 and fw56["wmfw"] and fw56["amp1"] and fw56["amp2"]:
                level = 2
                status = "TECHNICALLY_SUPPORTABLE"
                missing_reason = "MISSING_EMBEDDED_FW_AND_HW_VALIDATION"
                fw_files = sorted({resolved_whence[p] for p in (fw56["wmfw"] | fw56["amp1"] | fw56["amp2"])})
            elif has_b0_2amp_fw:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "MISSING_ACPI_DSD_OR_BOARD_PROPERTIES"
            elif fw56 and (fw56["wmfw"] or fw56["amp1"] or fw56["amp2"] or fw56["amplr"]):
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "UNVERIFIED_CHANNEL_COEFFICIENTS"
            else:
                level = 1
                status = "RECOGNIZED_CANDIDATE"
                missing_reason = "MISSING_UPSTREAM_FIRMWARE"

        provenance = []
        if in_prod:
            provenance.append("cirrusaudiofixup:readme+firmware")
        if in_l41_board:
            provenance.append("linux:cs35l41_hda_property")
        if in_l56_board:
            provenance.append("cirrusaudiofixup:cs35l56_board_props")
        if ssid in up_l56_fixups:
            provenance.append("linux:cs35l56_hda")
        if in_alc:
            provenance.append(f"linux:alc269:{in_alc['fixup']}")
        if fw41 or fw56:
            provenance.append("linux-firmware:WHENCE")

        entry_dict = {
            "subsystem": f"{vendor:04X}:{device:04X}",
            "subsystem_hex": f"0x{ssid:08X}",
            "oem": oem,
            "model": model,
            "family": family,
            "level": level,
            "status": status,
            "amp_count": amp_count,
            "bus_hint": bus,
            "upstream_firmware_available": fw_available,
            "missing_reason": missing_reason,
            "provenance": provenance,
        }
        if fw_files:
            entry_dict["upstream_firmware_files"] = fw_files
        entries.append(entry_dict)

    aliases = [
        {
            "from": f"{src >> 16:04X}:{src & 0xFFFF:04X}",
            "from_hex": f"0x{src:08X}",
            "to": f"{dst >> 16:04X}:{dst & 0xFFFF:04X}",
            "to_hex": f"0x{dst:08X}",
        }
        for src, dst in sorted(local["l41_aliases"].items())
        if src not in all_ssids and dst in all_ssids
    ]

    counts = {
        "total": len(entries),
        "level_3_production_supported": sum(1 for e in entries if e["level"] == 3),
        "level_2_technically_supportable": sum(1 for e in entries if e["level"] == 2),
        "level_1_recognized_candidate": sum(1 for e in entries if e["level"] == 1),
        "cs35l41": sum(1 for e in entries if e["family"] == "CS35L41"),
        "cs35l56": sum(1 for e in entries if e["family"] == "CS35L56"),
        "aliases": len(aliases),
    }
    catalog = {
        "schema_version": 1,
        "sources": {
            "linux_repo": "https://github.com/torvalds/linux",
            "linux_master_commit": linux_sha,
            "linux_master_date": linux_date,
            "linux_firmware_whence": "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/WHENCE",
        },
        "summary": counts,
        "aliases": aliases,
        "entries": entries,
    }
    CATALOG_JSON.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8", newline="\n")
    CATALOG_HPP.write_text(render_catalog_hpp(catalog), encoding="utf-8", newline="\n")
    print("Generated catalog:", json.dumps(counts))


def write_board_properties_hpp(rows: dict[int, dict]) -> None:
    lines = [
        "// Board facts from Linux sound/hda/codecs/side-codecs/cs35l41_hda_property.c.",
        "// GPIO fields are _CRS resource indices, never physical pins or MMIO addresses.",
        "// A table match does not authorize an unsupported topology or unmatched tuning.",
        "#pragma once",
        "#include <stdint.h>",
        "namespace cirrus::devices::amplifiers::cs35l41 {",
        "    struct BoardProperties {",
        "        uint32_t subsystem;",
        "        uint8_t count;",
        "        bool internalBoost;",
        "        uint8_t channels[4];",
        "        int8_t resetResource, speakerResource, csResource;",
        "        uint32_t inductor, peak, capacitor;",
        "        constexpr uint32_t boostType() const { return internalBoost ? 0 : 1; }",
        "        constexpr uint32_t gpio1Function() const { return boostType() == 1 ? 1 : 0; }",
        "    };",
        "    inline constexpr BoardProperties kBoardProperties[] = {",
    ]
    for ssid in sorted(rows):
        r = rows[ssid]
        ch = ", ".join(str(x) for x in r["channels"])
        ib = "true" if r["internal_boost"] else "false"
        lines.append(
            f"        {{0x{ssid:08x}, {r['count']}, {ib}, {{{ch}}}, {r['reset_resource']}, {r['speaker_resource']}, {r['cs_resource']}, {r['inductor_nh']}, {r['peak_ma']}, {r['capacitor_uf']}}},"
        )
    lines.extend(
        [
            "    };",
            "    inline const BoardProperties* boardProperties(uint16_t vendor, uint16_t device) {",
            "        const uint32_t subsystem = uint32_t(vendor) << 16 | device;",
            "        for (const auto& properties : kBoardProperties) {",
            "            if (properties.subsystem == subsystem) {",
            "                return &properties;",
            "            }",
            "        }",
            "        return nullptr;",
            "    }",
            "}",
        ]
    )
    BOARD_PROPS_HPP.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


MISSING_REASON_ENUM = {
    "NONE": ("MissingReason::None", "NONE"),
    "MISSING_EMBEDDED_FW_AND_HW_VALIDATION": (
        "MissingReason::MissingEmbeddedFwAndHwValidation",
        "MISSING_EMBEDDED_FW_AND_HW_VALIDATION",
    ),
    "UNSUPPORTED_QUAD_AMP": ("MissingReason::UnsupportedQuadAmp", "UNSUPPORTED_QUAD_AMP"),
    "UNSUPPORTED_SINGLE_AMP": ("MissingReason::UnsupportedSingleAmp", "UNSUPPORTED_SINGLE_AMP"),
    "UNSUPPORTED_SILICON_REV_B2": ("MissingReason::UnsupportedSiliconRevB2", "UNSUPPORTED_SILICON_REV_B2"),
    "SPI_REQUIRES_NATIVE_ACPI_DSD_AND_FW": (
        "MissingReason::SpiRequiresNativeAcpiDsdAndFw",
        "SPI_REQUIRES_NATIVE_ACPI_DSD_AND_FW",
    ),
    "AMBIGUOUS_BUS_TOPOLOGY": ("MissingReason::AmbiguousBusTopology", "AMBIGUOUS_BUS_TOPOLOGY"),
    "REQUIRES_NATIVE_ACPI_DSD_OR_METADATA": (
        "MissingReason::RequiresNativeAcpiDsdOrMetadata",
        "REQUIRES_NATIVE_ACPI_DSD_OR_METADATA",
    ),
    "MISSING_ACPI_DSD_OR_BOARD_PROPERTIES": (
        "MissingReason::MissingAcpiDsdOrBoardProperties",
        "MISSING_ACPI_DSD_OR_BOARD_PROPERTIES",
    ),
    "MISSING_BOARD_METADATA": ("MissingReason::MissingBoardMetadata", "MISSING_BOARD_METADATA"),
    "MISSING_UPSTREAM_FIRMWARE": ("MissingReason::MissingUpstreamFirmware", "MISSING_UPSTREAM_FIRMWARE"),
    "UNVERIFIED_CHANNEL_COEFFICIENTS": (
        "MissingReason::UnverifiedChannelCoefficients",
        "UNVERIFIED_CHANNEL_COEFFICIENTS",
    ),
    "SPEAKER_ID_GPIO_MISMATCH": ("MissingReason::SpeakerIdGpioMismatch", "SPEAKER_ID_GPIO_MISMATCH"),
    "UNRECOGNIZED_SUBSYSTEM_ID": ("MissingReason::UnrecognizedSubsystemId", "UNRECOGNIZED_SUBSYSTEM_ID"),
    "NO_SUBSYSTEM_ID": ("MissingReason::NoSubsystemId", "NO_SUBSYSTEM_ID"),
}


def render_catalog_hpp(catalog: dict) -> str:
    entries = catalog["entries"]
    aliases = catalog.get("aliases", [])
    lines = [
        "// Auto-generated by Tools/Hardware/sync_hardware_catalog.py.",
        "// Read-only diagnostic hardware catalog: NEVER used to authorize boards or bypass safety checks.",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "namespace cirrus::platform::catalog {",
        "",
        "    enum class AmplifierFamily : uint8_t { Unknown = 0, CS35L41 = 1, CS35L56 = 2 };",
        "    enum class SupportLevel : uint8_t {",
        "        Unknown = 0,",
        "        RecognizedCandidate = 1,",
        "        TechnicallySupportable = 2,",
        "        ProductionSupported = 3,",
        "    };",
        "    enum class MissingReason : uint8_t {",
        "        None = 0,",
        "        MissingEmbeddedFwAndHwValidation,",
        "        UnsupportedQuadAmp,",
        "        UnsupportedSingleAmp,",
        "        UnsupportedSiliconRevB2,",
        "        SpiRequiresNativeAcpiDsdAndFw,",
        "        AmbiguousBusTopology,",
        "        RequiresNativeAcpiDsdOrMetadata,",
        "        MissingAcpiDsdOrBoardProperties,",
        "        MissingBoardMetadata,",
        "        MissingUpstreamFirmware,",
        "        UnverifiedChannelCoefficients,",
        "        SpeakerIdGpioMismatch,",
        "        UnrecognizedSubsystemId,",
        "        NoSubsystemId,",
        "    };",
        "",
        "    struct CandidateEntry {",
        "        uint32_t subsystem;",
        "        AmplifierFamily family;",
        "        SupportLevel level;",
        "        uint8_t ampCount;",
        "        MissingReason missingReason;",
        "    };",
        '    static_assert(sizeof(CandidateEntry) == 8, "CandidateEntry must remain compact");',
        "",
        "    struct SubsystemAlias {",
        "        uint32_t fromSubsystem;",
        "        uint32_t toSubsystem;",
        "    };",
        "",
        "    inline const char* familyName(AmplifierFamily family) {",
        "        switch (family) {",
        '        case AmplifierFamily::CS35L41: return "CS35L41";',
        '        case AmplifierFamily::CS35L56: return "CS35L56";',
        '        default: return "UNKNOWN";',
        "        }",
        "    }",
        "",
        "    inline const char* supportLevelName(SupportLevel level) {",
        "        switch (level) {",
        '        case SupportLevel::ProductionSupported: return "PRODUCTION_SUPPORTED";',
        '        case SupportLevel::TechnicallySupportable: return "TECHNICALLY_SUPPORTABLE_NO_EMBEDDED_FW";',
        '        case SupportLevel::RecognizedCandidate: return "RECOGNIZED_CANDIDATE";',
        '        default: return "UNKNOWN_HARDWARE";',
        "        }",
        "    }",
        "",
        "    inline const char* missingReasonName(MissingReason reason) {",
        "        switch (reason) {",
    ]
    for enum_expr, label in MISSING_REASON_ENUM.values():
        lines.append(f'        case {enum_expr}: return "{label}";')
    lines.extend(
        [
            '        default: return "UNRECOGNIZED_SUBSYSTEM_ID";',
            "        }",
            "    }",
            "",
            "    inline constexpr SubsystemAlias kSubsystemAliases[] = {",
        ]
    )
    for alias in aliases:
        lines.append(
            f'        {{0x{int(alias["from_hex"], 16):08X}U, 0x{int(alias["to_hex"], 16):08X}U}},'
        )
    lines.extend(
        [
            "    };",
            "",
            "    inline constexpr CandidateEntry kHardwareCatalog[] = {",
        ]
    )
    for e in entries:
        fam = f"AmplifierFamily::{e['family']}"
        lvl_map = {
            1: "SupportLevel::RecognizedCandidate",
            2: "SupportLevel::TechnicallySupportable",
            3: "SupportLevel::ProductionSupported",
        }
        lvl = lvl_map[e["level"]]
        reason_enum, _ = MISSING_REASON_ENUM[e["missing_reason"]]
        lines.append(
            f'        {{0x{int(e["subsystem_hex"], 16):08X}U, {fam}, {lvl}, {e["amp_count"]}, {reason_enum}}},'
        )
    lines.extend(
        [
            "    };",
            "",
            "    inline constexpr size_t kHardwareCatalogCount = sizeof(kHardwareCatalog) / sizeof(kHardwareCatalog[0]);",
            "",
            "    inline const CandidateEntry* lookupCandidate(uint32_t subsystem) {",
            "        if (!subsystem) {",
            "            return nullptr;",
            "        }",
            "        for (const auto& alias : kSubsystemAliases) {",
            "            if (alias.fromSubsystem == subsystem) {",
            "                subsystem = alias.toSubsystem;",
            "                break;",
            "            }",
            "        }",
            "        size_t lo = 0;",
            "        size_t hi = kHardwareCatalogCount;",
            "        while (lo < hi) {",
            "            const size_t mid = lo + (hi - lo) / 2;",
            "            const uint32_t current = kHardwareCatalog[mid].subsystem;",
            "            if (current == subsystem) {",
            "                return &kHardwareCatalog[mid];",
            "            }",
            "            if (current < subsystem) {",
            "                lo = mid + 1;",
            "            } else {",
            "                hi = mid;",
            "            }",
            "        }",
            "        return nullptr;",
            "    }",
            "",
            "}",
        ]
    )
    return "\n".join(lines) + "\n"


def verify_offline() -> None:
    assert CATALOG_JSON.is_file(), f"Missing {CATALOG_JSON}"
    assert CATALOG_HPP.is_file(), f"Missing {CATALOG_HPP}"
    catalog = json.loads(CATALOG_JSON.read_text(encoding="utf-8"))
    assert catalog.get("schema_version") == 1
    entries = catalog["entries"]
    aliases = catalog.get("aliases", [])
    assert len(entries) > 0

    seen = set()
    prev = -1
    for e in entries:
        ssid = int(e["subsystem_hex"], 16)
        assert ssid > prev, f"Catalog not strictly sorted at {e['subsystem_hex']}"
        prev = ssid
        assert ssid not in seen, f"Duplicate catalog SSID {e['subsystem_hex']}"
        seen.add(ssid)
        assert e["family"] in ("CS35L41", "CS35L56")
        assert e["level"] in (1, 2, 3)
        assert e["missing_reason"] in MISSING_REASON_ENUM
        if e["level"] == 2:
            assert e["bus_hint"] == "I2C", f"Level 2 board {e['subsystem']} must be confirmed I2C, got {e['bus_hint']}"
            assert e["amp_count"] == 2, f"Level 2 board {e['subsystem']} must be 2-amp"
            assert e["upstream_firmware_available"] is True
            assert e.get("upstream_firmware_files"), f"Level 2 board {e['subsystem']} missing firmware file list"

    for alias in aliases:
        from_id = int(alias["from_hex"], 16)
        to_id = int(alias["to_hex"], 16)
        assert from_id not in seen, f"Alias source {alias['from']} duplicates primary entry"
        assert to_id in seen, f"Alias target {alias['to']} missing from catalog"

    local = parse_local_production_sources()
    prod_ssids = set(local["readme_models"].keys())
    level3_ssids = {int(e["subsystem_hex"], 16) for e in entries if e["level"] == 3}
    assert level3_ssids == prod_ssids, (
        f"Level 3 mismatch: catalog={[hex(x) for x in sorted(level3_ssids)]} vs readme={[hex(x) for x in sorted(prod_ssids)]}"
    )

    for ssid in local["l41_board_props"]:
        assert ssid in seen, f"BoardProperties SSID 0x{ssid:08X} missing from catalog"

    expected_hpp = render_catalog_hpp(catalog)
    actual_hpp = CATALOG_HPP.read_text(encoding="utf-8")
    assert actual_hpp == expected_hpp, "HardwareCatalog.hpp is out of sync with hardware_catalog.json"

    s = catalog["summary"]
    assert s["total"] == len(entries)
    assert s["level_3_production_supported"] == len(prod_ssids)
    assert s["level_2_technically_supportable"] == sum(1 for e in entries if e["level"] == 2)
    assert s["level_1_recognized_candidate"] == sum(1 for e in entries if e["level"] == 1)
    assert s.get("aliases", 0) == len(aliases)
    print(
        f"PASS hardware catalog verify: {s['total']} distinct boards "
        f"(L3={s['level_3_production_supported']}, L2={s['level_2_technically_supportable']}, L1={s['level_1_recognized_candidate']}, aliases={len(aliases)})"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="Fetch latest upstream Linux & WHENCE data")
    parser.add_argument("--verify", action="store_true", help="Verify local catalog consistency offline")
    args = parser.parse_args()
    if not args.fetch and not args.verify:
        parser.error("Specify --fetch or --verify")
    if args.fetch:
        fetch_and_generate()
    if args.verify:
        verify_offline()
    return 0


if __name__ == "__main__":
    sys.exit(main())
