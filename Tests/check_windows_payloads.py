"""Test payload comparison without requiring OEM files."""
import importlib.util
import base64
import struct
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("windows_audit", ROOT / "Tools/audit_windows_payloads.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    header = root / "Firmware.hpp"
    header.write_text("const uint8_t sample[] = {0x57, 0x4d, 0x46, 0x57, 0};\n", encoding="utf-8")
    packages = root / "packages"
    packages.mkdir()
    payload = packages / "sample.wmfw"
    payload.write_bytes(b"WMFW\0")
    inf = packages / "sample.inf"
    inf.write_text("[Settings]\nHKR,Settings,DefaultTuningSet,%REG_DWORD%,4\n", encoding="utf-16")
    driver = root / "sample.sys"
    driver.write_bytes(b"VolumeRampRate\0\0" + "EnableHibernation".encode("utf-16le"))
    before = {p: p.read_bytes() for p in (header, payload, inf, driver)}
    profile = root / "speaker.nsx"
    encoded = base64.b64encode(struct.pack("<ff", 1.0, -0.25)).decode("ascii")
    profile.write_text(
        "<nhSettings><Settings>" +
        "".join(f"<kSet_{name}><Value>{encoded}</Value></kSet_{name}>" for name in
                ("DeviceOptimizationFilterFL", "DeviceOptimizationFilterFR")) +
        '<kSet_MasterOutputGainDB Value="0" />'
        "</Settings></nhSettings>", encoding="utf-8")
    original = profile.read_bytes()
    acoustic = module.audit_nahimic(profile)
    assert acoustic["optimization_filters_byte_identical"]
    hypothesis = acoustic["fields"]["DeviceOptimizationFilterFL"]["float32le_hypothesis"]
    assert hypothesis["count"] == 2 and hypothesis["sum"] == 0.75
    assert hypothesis["minimum"] == -0.25
    assert acoustic["fields"]["MasterOutputGainDB"]["value"] == "0"
    assert profile.read_bytes() == original
    profile.write_text("<Settings><DeviceOptimizationFilterFL><Value>!invalid!</Value>"
                       "</DeviceOptimizationFilterFL></Settings>", encoding="utf-8")
    try:
        module.audit_nahimic(profile)
    except ValueError:
        pass
    else:
        raise AssertionError("Invalid base64 accepted")
    result = module.audit(header, [packages], [driver])
    assert result["embedded_payloads"][0]["exact_matches"] == [str(payload)]
    assert result["embedded_payloads"][0]["size"] == 5
    assert result["inf_evidence"][0]["line"] == 2
    assert result["inf_evidence"][0]["section"] == "Settings"
    inf.write_text('[CONF_0100.Tunings.AddReg]\n'
                   'HKR,Tunings\\B2\\4\\1\\0,,%REG_SZ%,Veco_Left.bin\n'
                   '[OtherBoard]\nHKR,Settings,Gain,%REG_DWORD%,123\n', encoding="utf-16")
    scoped = module.audit(header, [packages], [])["inf_evidence"]
    assert scoped[0]["section"] == "CONF_0100.Tunings.AddReg"
    assert "Veco_Left.bin" in scoped[0]["text"]
    assert scoped[1]["section"] == "OtherBoard"
    inf.write_bytes(before[inf])
    assert "VolumeRampRate" in result["driver_evidence"][0]["strings"]
    assert "EnableHibernation" in result["driver_evidence"][0]["strings"]
    assert all(p.read_bytes() == data for p, data in before.items())
    payload.write_bytes(b"WMFW\1")
    assert not module.audit(header, [packages], [])["embedded_payloads"][0]["exact_matches"]
    try:
        module.audit(header, [root / "absent"], [])
    except ValueError:
        pass
    else:
        raise AssertionError("Missing directory accepted")
    header.write_text("const uint8_t sample[] = {1 << 2};", encoding="utf-8")
    try:
        module.embedded_payloads(header)
    except ValueError:
        pass
    else:
        raise AssertionError("Unsupported byte expression accepted")

print("PASS Windows payload audit: hashes, UTF-16 INF, strings, failures and read-only behavior")
