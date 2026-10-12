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

# Chip state must compile through host lifecycle fixtures without IOService.
state = (ROOT / "Devices/CS35L41/State.hpp").read_text()
service = (ROOT / "CirrusAudioFixup.hpp").read_text()
assert "struct State {" in state
assert "struct AmplifierState : cirrus::devices::cs35l41::State" in service
for include in re.findall(r'^#include "([^"]+)"', state, re.M):
    assert not include.startswith(("Platform/", "Transport/")), include
assert not re.search(r"#include [<\"](?:IOKit|CirrusAudioFixup)", state)
for field in ("registerIO", "endpointLabel", "endpointIndex", "channel", "model"):
    assert not re.search(r"\b" + field + r"\s*[{;]", state), field
for field in ("playbackTransition", "shutdownTransition", "firmwareValidated", "diagnostic", "pllRecoveryAttempts"):
    assert re.search(r"\b" + field + r"\s*[{;]", state), field
    assert not re.search(r"\b" + field + r"\s*[{;]", service), field
print("PASS CS35L41 state schema is independent of IOService and endpoint ownership")

backend = (ROOT / "Devices/CS35L41/CS35L41Backend.cpp").read_text()
assert '"CirrusAudioFixup.hpp"' not in backend
assert "CirrusAudioFixup::" not in backend and "mOwner" not in backend
assert "Platform& mPlatform" in backend and "State& mAmplifier" in backend
assert "class CS35L41Backend;" not in service
assert "friend cirrus::core::AmplifierLifecycleHooks*" not in service
assert "struct AmplifierBackendOwner final : cirrus::devices::cs35l41::Platform" in service
print("PASS standalone CS35L41 backend and explicit service platform adapter boundary")

# Lifecycle orchestration must stay in the chip backend, rather than returning
# to a service callback under a new name.
contract = (ROOT / "Devices/CS35L41/CS35L41Backend.hpp").read_text()
service_cpp = (ROOT / "CirrusAudioFixup.cpp").read_text()
for name in ("initializeCS35L41Hardware", "loadCS35L41Firmware", "verifyCS35L41Ready", "shutdownCS35L41", "stopCS35L41At"):
    assert name not in service and name not in service_cpp, name
for name in ("initializeHardware", "loadFirmwareFlow", "verifyReadyFlow", "shutdownFlow"):
    assert "IOReturn Backend::" + name + "()" in backend, name
for name in ("initialize", "loadFirmware", "verifyReady", "shutdown"):
    assert not re.search(r"virtual IOReturn " + name + r"\(", contract), name
print("PASS CS35L41 initialization, firmware, readiness and shutdown orchestration are backend-owned")

# Framework target: ordinary backends and provider-backed communication adapters
# must not need a descriptor/model branch in manifest-backed discovery.
host = (ROOT / "Platform/AmplifierHost.hpp").read_text()
assert "Devices/" not in host and "CS35L41" not in host and "WMFW" not in host
assert "AmplifierHost* host" in registry and "const char* platformContract" in registry
configured_i2c = service_cpp.split("size_t CirrusAudioFixup::detectConfiguredI2CAmplifiers() {", 1)[1].split("\n}", 1)[0]
assert "findAmplifierById" not in configured_i2c
assert "backend->model" in configured_i2c and "backend->identityMask" in configured_i2c
assert "cirrus::transport::I2CController* mI2CController" in service
assert "i2cControllerFactory" in (ROOT / "Platform/PlatformProfile.hpp").read_text()
assert "definition->platformContract?static_cast" in service_cpp
print("PASS registry-owned discovery, generic amplifier host and replaceable I2C communication owner")

composition = (ROOT / "Devices/AmplifierBackends.hpp").read_text()
assert "cs35l41::registerBackends(registry)" in composition
assert "0x35a40" not in composition and "RegisterFormat" not in composition and "RegisterAccess" not in composition
assert "kBackendDefinitions" in (ROOT / "Devices/CS35L41/Definition.hpp").read_text()
print("PASS AMP-owned registration/framing/access metadata and shared module composition")
