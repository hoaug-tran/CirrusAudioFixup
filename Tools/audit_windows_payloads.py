"""Read-only comparison of embedded CS35L41 payloads and OEM Windows files.

Run against an exported driver directory. Matching bytes establish payload
identity, not active Windows profile selection or acoustic equivalence.
"""

import argparse
import base64
import hashlib
import json
import math
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def embedded_payloads(header):
    text = header.read_text(encoding="utf-8")
    arrays = {}
    for match in re.finditer(
        r"const\s+uint8_t\s+(\w+)\s*\[\]\s*=\s*\{([^}]*)\}\s*;", text
    ):
        body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match[2], flags=re.S)
        values = [value.strip() for value in body.split(",") if value.strip()]
        # Refuse expressions instead of guessing their compiled byte value.
        if any(not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", v) for v in values):
            raise ValueError(f"Unsupported byte expression in {match[1]}")
        data = bytes(int(v, 16 if v.lower().startswith("0x") else 10) for v in values)
        if data[:4] in (b"WMFW", b"WMDR"):
            arrays[match[1]] = data
    return arrays


def binary_strings(data):
    # Keep both Windows Unicode strings and ordinary ASCII diagnostic labels.
    ascii_strings = [m[0].decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{6,}", data)]
    wide_strings = [
        m[0].decode("utf-16le")
        for m in re.finditer(rb"(?:[\x20-\x7e]\x00){6,}", data)
    ]
    terms = re.compile(
        r"calibr|hibern|mailbox|interrupt|playback|ramp|tuning|gain|power|speaker|\.pdb", re.I
    )
    return sorted({s for s in ascii_strings + wide_strings if terms.search(s)})


def audit_nahimic(path):
    """Describe an OEM profile without treating its private format as a DSP ABI.

    Float interpretation is deliberately labelled as a hypothesis. Neither the
    sample rate nor the filter layout is encoded by the base64 wrapper itself.
    This report must never be used as a register-write recipe.
    """
    data = path.read_bytes()
    root = ET.fromstring(data)
    fields = {}
    for setting in root.iter():
        value = setting.find("Value")
        text = setting.get("Value")
        if text is None and value is not None:
            text = value.text
        if text is None:
            continue
        name = setting.tag.removeprefix("kSet_")
        text = text.strip()
        if not any(term in name for term in
                   ("Filter", "Attenuator", "Gain", "Limiter", "BassBoost")):
            continue
        entry = {"value": text}
        if "FilterF" in name or "ThresholdDB" in name or "FrequencyResponse" in name:
            payload = base64.b64decode(text, validate=True)
            entry = {"bytes": len(payload), "sha256": digest(payload)}
            if payload and len(payload) % 4 == 0:
                values = struct.unpack("<" + "f" * (len(payload) // 4), payload)
                finite = all(math.isfinite(v) for v in values)
                entry["float32le_hypothesis"] = {
                    "count": len(values), "all_finite": finite,
                    "minimum": min(values) if finite else None,
                    "maximum": max(values) if finite else None,
                    "sum": math.fsum(values) if finite else None,
                }
        fields[name] = entry
    left = fields.get("DeviceOptimizationFilterFL", {})
    right = fields.get("DeviceOptimizationFilterFR", {})
    return {"path": str(path), "sha256": digest(data), "fields": fields,
            "optimization_filters_byte_identical": bool(left.get("sha256")) and
            left.get("sha256") == right.get("sha256"),
            "boundary": "Static OEM settings only. Float layout, sample rate, frequency bins, "
                        "active APO selection and acoustic response are not established."}


def audit(header, roots, drivers):
    arrays = embedded_payloads(header)
    candidates = {}
    configurations = []
    for root in roots:
        if not root.is_dir():
            raise ValueError(f"Missing package directory: {root}")
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix.lower() in (".wmfw", ".bin", ".bincfg"):
                data = path.read_bytes()
                candidates.setdefault(digest(data), []).append(str(path))
            elif path.suffix.lower() == ".inf":
                data = path.read_bytes()
                encoding = "utf-16" if data[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig"
                section = None
                for number, line in enumerate(data.decode(encoding, errors="replace").splitlines(), 1):
                    # Section provenance matters: an INF can contain gains and
                    # profiles for many unrelated boards. These are candidate
                    # settings, not proof of the installed driver's selection.
                    heading = re.fullmatch(r"\s*\[([^]]+)\]\s*(?:;.*)?", line)
                    if heading:
                        section = heading[1]
                    if re.search(r"CLSA.*0100|3847|Gain,|HKR,\s*Tunings\\|VolumeRampRate|DefaultTuningSet|EnableHibernation|EnableIntHandler|EnableAutoCal|ForceAmpSafeState", line, re.I):
                        configurations.append({"path": str(path), "line": number,
                                               "section": section, "text": line.strip()})
    payloads = []
    for name, data in arrays.items():
        checksum = digest(data)
        payloads.append({"symbol": name, "size": len(data), "sha256": checksum,
                         "exact_matches": candidates.get(checksum, [])})
    binaries = []
    for path in drivers:
        data = path.read_bytes()
        binaries.append({"path": str(path), "size": len(data), "sha256": digest(data),
                         "strings": binary_strings(data)})
    return {"embedded_payloads": payloads, "inf_evidence": configurations,
            "driver_evidence": binaries,
            "boundary": "Byte identity and static configuration only; no runtime selection or acoustic proof."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=Path(__file__).resolve().parents[1] /
                        "CirrusAudioFixup/Devices/CS35L41/Resources/Firmware.hpp")
    parser.add_argument("--packages", type=Path, nargs="+", required=True)
    parser.add_argument("--driver", type=Path, action="append", default=[])
    parser.add_argument("--nahimic", type=Path, action="append", default=[],
                        help="Extracted OEM .nsx profile; inspected read-only")
    args = parser.parse_args()
    try:
        result = audit(args.header, args.packages, args.driver)
        result["nahimic_evidence"] = [audit_nahimic(path) for path in args.nahimic]
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
