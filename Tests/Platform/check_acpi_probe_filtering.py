import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

test_code = r'''
#define KERNEL 1
#include "Fixture.hpp"
#include "Platform/ACPI/ACPISPIResources.hpp"
#include "Support/KernelDeviceCast.hpp"

using SInt32 = int32_t;
bool gCirrusDebug = true;

class MockRoot : public IOService {};
static MockRoot gMockRoot;

class CirrusAudioFixup : public IOService {
public:
    IOService* probe(IOService* provider, SInt32* score) {
        if (!provider)
            return nullptr;

        if (auto* acpi = cirrus::support::acpiDeviceCast(provider)) {
            OSObject* resources = nullptr;
            const IOReturn resourceReturn = acpi->evaluateObject("_CRS", &resources);
            auto* resourceData = OSDynamicCast(OSData, resources);
            cirrus::transport::ACPISPIResources parsed{};
            bool hasI2C = false;
            const auto resourceKind = resourceReturn == kIOReturnSuccess && resourceData
                                          ? cirrus::transport::parseACPISPIResources(static_cast<const uint8_t*>(resourceData->getBytesNoCopy()),
                                                                                     resourceData->getLength(), parsed, &hasI2C)
                                          : cirrus::transport::ACPISPIResult::Malformed;
            OSSafeReleaseNULL(resources);
            if (resourceKind != cirrus::transport::ACPISPIResult::Success)
                return nullptr;
        }

        setProperty("CirrusReachedProbe", "true");
        if (score)
            *score += 9000;
        return this;
    }
};

std::vector<uint8_t> make_i2c_crs() {
    std::string source = "\\_SB.I2CD";
    std::vector<uint8_t> d(18 + source.size() + 1);
    d[0] = 0x8e;
    d[1] = uint8_t(d.size() - 3);
    d[3] = 1;
    d[5] = 1;
    d[6] = 2;
    d[9] = 1;
    d[10] = 6;
    d[16] = 0x40;
    memcpy(d.data() + 18, source.c_str(), source.size() + 1);
    d.push_back(0x79);
    d.push_back(0);
    return d;
}

std::vector<uint8_t> make_spi_crs(unsigned cs = 1, unsigned mode = 0) {
    std::vector<uint8_t> d = {0x8e, 0, 0, 1, 0, 2, 2, 0, 0, 1, 9, 0, 0x40, 0x42, 0x0f, 0, 8,
                              uint8_t(mode & 1), uint8_t(mode >> 1), uint8_t(cs), 0};
    const char path[] = "\\_SB.PC00.SPI1";
    d.insert(d.end(), path, path + sizeof(path));
    d[1] = uint8_t(d.size() - 3);
    d.push_back(0x79);
    d.push_back(0);
    return d;
}

int main() {
    IOACPIPlatformDevice i2cDevice;
    auto i2cBytes = make_i2c_crs();
    i2cDevice.values["_CRS"] = make<OSData>(i2cBytes);

    IOACPIPlatformDevice spiDevice;
    auto spiBytes = make_spi_crs();
    spiDevice.values["_CRS"] = make<OSData>(spiBytes);

    IOService nonAcpiDevice;

    CirrusAudioFixup fixup;
    SInt32 score = 1000;

    IOService* i2cProbeResult = fixup.probe(&i2cDevice, &score);
    assert(i2cProbeResult == nullptr);

    score = 1000;
    IOService* spiProbeResult = fixup.probe(&spiDevice, &score);
    assert(spiProbeResult == &fixup);
    assert(score == 10000);

    score = 10000;
    IOService* nonAcpiProbeResult = fixup.probe(&nonAcpiDevice, &score);
    assert(nonAcpiProbeResult == &fixup);
    assert(score == 19000);

    gMockRoot.setProperty("Cirrus_Failed_Service_Evidence", "FAILED_ATTACH");
    assert(gMockRoot.strings["Cirrus_Failed_Service_Evidence"] == "FAILED_ATTACH");

    gMockRoot.removeProperty("Cirrus_Failed_Service_Evidence");
    assert(!gMockRoot.strings.count("Cirrus_Failed_Service_Evidence"));

    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cirrus-probe-filter-") as directory:
    tmp = Path(directory)
    fixture_path = ROOT / "Tests/Fixtures"
    import sys
    sys.path.insert(0, str(fixture_path))
    from acpi_kernel_fixture import install
    install(tmp)

    src = tmp / "test.cpp"
    src.write_text(test_code)
    exe = tmp / "test.exe"

    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), "-I", str(ROOT / "CirrusAudioFixup"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print("PASS acpi probe filtering for i2c devices and evidence cleanup")
