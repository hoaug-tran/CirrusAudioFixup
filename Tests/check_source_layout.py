"""Check source naming and dependency boundaries used for amplifier expansion."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[1] / "CirrusAudioFixup"
files = sorted(p for p in ROOT.rglob("*") if p.suffix in {".hpp", ".cpp"})
for path in files:
    assert re.fullmatch(r"[A-Z][A-Za-z0-9]*", path.stem), path
    source = path.read_text()
    for include in re.findall(r'^#include "([^"]+)"', source, re.M):
        assert (ROOT / include).is_file() or (path.parent / include).is_file(), (path, include)
    if path.suffix == ".cpp":
        assert re.search(r"(?:class |::|namespace )", source), ("empty implementation", path)
assert not list((ROOT / "Transport").glob("CS35L*.hpp")), "silicon-specific framing belongs in Devices/<Model>"
provider = (ROOT / "Transport/VoodooI2CProvider.hpp").read_text()
assert "core::RegisterIO" not in provider and "writeBigEndian32" not in provider
assert not re.search(r"virtual[^;]+(?:wmfw|binSize)", (ROOT / "Core/AmplifierDevice.hpp").read_text()), "generic helpers must not require WMFW/BIN"
assert (ROOT / "Transport/I2CRegisterBus.hpp").is_file()
assert (ROOT / "Transport/SPIRegisterBus.hpp").is_file()
for name in ("AmplifierDevice", "AmplifierDescriptor", "AmplifierModel", "BusEndpoint"):
    assert any(re.search(r"(?:class|struct|enum class) " + name + r"\b", p.read_text()) for p in (ROOT / "Core").glob("*.hpp")), name
registry = (ROOT / "Platform/AmplifierBackendRegistry.hpp").read_text()
assert "class AmplifierBackendRegistry" in registry
for obsolete in ("Transport/VoodooI2CTransport.hpp", "Transport/CS35L41SPITransport.hpp", "Platform/HDA/HDAController.cpp"):
    assert not (ROOT / obsolete).exists(), obsolete
print("PASS source naming, quoted include resolution, controller/bus split and silicon framing boundary")
