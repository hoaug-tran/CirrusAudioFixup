"""Check source naming and dependency boundaries used for amplifier expansion."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from production_source_fixture import service_source, driver_source
import re
ROOT = Path(__file__).resolve().parents[2] / "CirrusAudioFixup"
files = sorted(p for p in ROOT.rglob("*") if p.suffix in {".hpp", ".cpp"})
for path in files:
    assert re.fullmatch(r"[A-Z][A-Za-z0-9]*", path.stem), path
    source = path.read_text()
    assert all("backend" not in part.lower() for part in path.relative_to(ROOT).parts), ("use the actual driver/controller role", path)
    assert not re.search(r"\b[A-Za-z0-9_]*Backend[A-Za-z0-9_]*\b", source), path
    for include in re.findall(r'^#include "([^"]+)"', source, re.M):
        assert (ROOT / include).is_file() or (path.parent / include).is_file(), (path, include)
    if path.suffix == ".cpp" and path.name != "FirmwareData.cpp":
        assert re.search(r"(?:class |::|namespace )", source), ("empty implementation", path)
assert not list((ROOT / "Transport").glob("CS35L*.hpp")), "silicon-specific framing belongs in Devices/<Model>"
provider = (ROOT / "Transport/I2C/VoodooI2CProvider.hpp").read_text()
assert "core::RegisterIO" not in provider and "writeBigEndian32" not in provider
assert not re.search(r"virtual[^;]+(?:wmfw|binSize)", (ROOT / "Core/AmplifierDevice.hpp").read_text()), "generic helpers must not require WMFW/BIN"
assert (ROOT / "Transport/I2C/I2CRegisterBus.hpp").is_file()
assert (ROOT / "Transport/SPI/SPIRegisterBus.hpp").is_file()
for name in ("AmplifierDevice", "AmplifierModel", "BusEndpoint"):
    assert any(re.search(r"(?:class|struct|enum class) " + name + r"\b", p.read_text()) for p in (ROOT / "Core").glob("*.hpp")), name
registry = (ROOT / "Runtime/AmplifierDriverRegistry.hpp").read_text()
assert "class AmplifierDriverRegistry" in registry
for obsolete in ("Transport/VoodooI2CTransport.hpp", "Transport/CS35L41SPITransport.hpp", "Platform/HDA/HDAController.cpp"):
    assert not (ROOT / obsolete).exists(), obsolete
print("PASS source naming, quoted include resolution, controller/bus split and silicon framing boundary")

# Chip state must compile through host lifecycle fixtures without IOService.
state = (ROOT / "Devices/Amplifiers/CS35L41/RuntimeState.hpp").read_text()
service = (ROOT / "Service/CirrusAudioFixup.hpp").read_text()
assert "struct RuntimeState {" in state
assert "struct AmplifierState {" in service and "cs35l41::RuntimeState" not in service
for include in re.findall(r'^#include "([^"]+)"', state, re.M):
    assert not include.startswith(("Platform/", "Transport/")), include
assert not re.search(r"#include [<\"](?:IOKit|CirrusAudioFixup)", state)
for field in ("registerIO", "endpointLabel", "endpointIndex", "channel", "model"):
    assert not re.search(r"\b" + field + r"\s*[{;]", state), field
for field in ("playbackTransition", "shutdownTransition", "firmwareValidated", "pllRecoveryAttempts"):
    assert re.search(r"\b" + field + r"\s*[{;]", state), field
    assert not re.search(r"\b" + field + r"\s*[{;]", service), field
print("PASS CS35L41 state schema is independent of IOService and endpoint ownership")

driver = (ROOT / "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.cpp").read_text()
assert '"Service/CirrusAudioFixup.hpp"' not in driver
assert "CirrusAudioFixup::" not in driver and "mOwner" not in driver
owned_header=(ROOT / "Devices/Amplifiers/CS35L41/CS35L41Driver.hpp").read_text()
owned_cpp=driver_source()
service_cpp=service_source()
assert "InstanceState mAmplifier" in owned_header and "RuntimeState& mAmplifier" not in owned_header
assert "class CS35L41Driver final : public core::AmplifierLifecycleHooks" in owned_header
assert "CirrusAudioFixup" not in owned_header and "CirrusAudioFixup::" not in owned_cpp
assert "struct AmplifierHostAdapter final : cirrus::platform::AmplifierHost" in service
assert "platformContext" not in owned_cpp and "static_cast<Platform" not in driver
assert "Operation::" not in service and "cs35l41::" not in service_cpp
for name in ("initializeHardware", "loadFirmwareFlow", "verifyReadyFlow", "shutdownFlow",
             "discoverFirmware", "applyCalibration", "monitorCS35L41", "sendMailboxCommand",
             "unpackOTP", "applyErrataPatch", "stopDSP", "verifyIdleConfiguration"):
    assert "CS35L41Driver::"+name+"(" in owned_cpp,name
    assert "CirrusAudioFixup::"+name+"(" not in service_cpp,name
    assert not re.search(r"\b"+name+r"\(",service),name
for field in ("wmfwData","firmwareIdentityPending","dspAlive","playbackTransition","pllRecoveryAttempts"):
    assert field not in service and field not in service_cpp,field
assert "transferToAddress" not in service_cpp and "writeBE32" not in service_cpp
print("PASS complete chip-owned state/firmware/silicon/runtime with generic host and register contracts")

# Framework target: ordinary drivers and provider-backed communication adapters
# must not need a descriptor/model branch in manifest-backed discovery.
host = (ROOT / "Runtime/AmplifierHost.hpp").read_text()
assert "Devices/" not in host and "CS35L41" not in host and "WMFW" not in host
assert "AmplifierHost* host" in registry and "const char* platformContract" in registry
configured_i2c = service_cpp.split("size_t CirrusAudioFixup::detectConfiguredI2CAmplifiers() {", 1)[1].split("\n}", 1)[0]
assert "findAmplifierById" not in configured_i2c
assert "driver->model" in configured_i2c and "driver->identityMask" in configured_i2c
assert "cirrus::transport::I2CController* mI2CController" in service
assert "i2cControllerFactory" in (ROOT / "Platform/Board/PlatformProfile.hpp").read_text()
assert "definition->platformContract?static_cast" not in service_cpp
print("PASS registry-owned discovery, generic amplifier host and replaceable I2C communication owner")

composition = (ROOT / "Devices/AmplifierDriverModules.hpp").read_text()
assert "cs35l41::registerDrivers(registry)" in composition
assert "0x35a40" not in composition and "RegisterFormat" not in composition and "RegisterAccess" not in composition
assert "kDriverRegistrations" in (ROOT / "Devices/Amplifiers/CS35L41/CS35L41DriverRegistration.hpp").read_text()
print("PASS AMP-owned registration/framing/access metadata and shared module composition")

# Communication ownership is a module contract as well as a transaction port.
native_host=(ROOT / "Transport/SPI/NativeSPIHost.hpp").read_text()
assert "mOwners" in native_host and "NativeSPIControllerFactory factory" in native_host
assert "mIntel" not in native_host and "mAMD" not in native_host
assert "IntelLPSSSPIController" not in native_host and "AMDSPIController" not in native_host
assert 'evaluateInteger("_PSC"' not in native_host
assert "spiControllerFactory" in (ROOT / "Platform/Board/PlatformProfile.hpp").read_text()
print("PASS generic native communication ownership/factory/PM with module-owned Intel/AMD policy")

firmware = ROOT / "Devices/Amplifiers/CS35L41/Resources"
assert (firmware / "FirmwareData.cpp").read_text().count('Resources/Generated/FirmwareData.inc"') == 1
assert sum('Resources/Generated/FirmwareData.inc"' in p.read_text() for p in files) == 1
assert "extern const FirmwareResource cs35l41Firmware[];" in (firmware / "FirmwareData.hpp").read_text()
assert "const uint8_t" not in (firmware / "FirmwareData.hpp").read_text()
