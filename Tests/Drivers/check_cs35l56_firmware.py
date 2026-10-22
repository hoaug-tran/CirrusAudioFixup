import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
from driver_kernel_fixture import install
from host_build import sanitizer_flags

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-cs35l56-firmware-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#include <cassert>
#include <memory>
#include <map>
#include <vector>
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareSelection.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"
#include "Devices/Amplifiers/CS35L56/DSPControl.hpp"
#undef CIRRUS_LOG
#undef CIRRUS_ERR
template <typename... Args> void fixtureLog(const char*, Args...) {}
#define CIRRUS_LOG(...) fixtureLog(__VA_ARGS__)
#define CIRRUS_ERR(...) fixtureLog(__VA_ARGS__)
#include "Firmware/WMFW/WMFWUploader.hpp"
using namespace cirrus::devices::amplifiers::cs35l56;
struct MemoryIO : cirrus::core::RegisterIO {
    std::map<uint32_t, uint8_t> memory;
    unsigned writes{0}, reads{0}, failWrite{0}, failRead{0};
    bool read(uint32_t, uint32_t*) override { return false; }
    bool write(uint32_t, uint32_t) override { return false; }
    bool updateBits(uint32_t, uint32_t, uint32_t) override { return false; }
    bool pollBit(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
    bool bulkRead(uint32_t reg, uint8_t* data, size_t size) override {
        if (++reads >= failRead && failRead) return false;
        for (size_t i = 0; i < size; ++i) data[i] = memory[reg + uint32_t(i)];
        return true;
    }
    bool bulkWrite(uint32_t reg, const uint8_t* data, size_t size) override {
        if (++writes >= failWrite && failWrite) return false;
        for (size_t i = 0; i < size; ++i) memory[reg + uint32_t(i)] = data[i];
        return true;
    }
};
int main() {
    assert(cs35l56FirmwareCount==1);
    auto* resource=selectFirmwareResource(cs35l56Firmware,cs35l56FirmwareCount,0x1043,0x1c63,0);
    assert(resource&&resource->wmfwSize==31996&&resource->binSize==8192&&resource->binRightSize==8192);
    assert(!selectFirmwareResource(cs35l56Firmware,cs35l56FirmwareCount,0x1043,0x1c63,1));
    assert(!selectFirmwareResource(cs35l56Firmware,cs35l56FirmwareCount,0x1043,0xffff,0));
    assert(memcmp(resource->bin,resource->binRight,8192));
    auto image=std::make_unique<FirmwareImage>();
    assert(WMFWParser::parseWMFW(resource->wmfw,resource->wmfwSize,image.get()));
    assert(image->regionCount==70&&image->wmfwAlgorithmCount==42&&image->wmfwControlCount==189);
    auto mapped = std::make_unique<MappedImage>();
    assert(WMFWMemoryMapper::mapFirmwareImage(*image, *mapped));
    assert(mapped->regionCount == image->regionCount);
    unsigned memoryRegions = 0;
    for (uint32_t i = 0; i < mapped->regionCount; ++i) {
        const auto& region = mapped->regions[i];
        assert(region.size && region.data.begin && region.data.size == region.size);
        assert(region.data.begin >= resource->wmfw);
        assert(region.data.begin + region.size <= resource->wmfw + resource->wmfwSize);
        if (region.dspRegister != 0xffffffff) {
            uint32_t limit = 0;
            switch (region.regionType) {
            case RegionType::XM_PACKED: limit = 0x02006000; break;
            case RegionType::XM_UNPACKED: limit = 0x02808000; break;
            case RegionType::YM_PACKED: limit = 0x02c047f4; break;
            case RegionType::YM_UNPACKED: limit = 0x03405ff8; break;
            case RegionType::PM_PACKED: limit = 0x03804fec; break;
            default: assert(false);
            }
            assert(uint64_t(region.dspRegister) + region.size <= limit);
            auto plan = std::make_unique<UploadPlan>();
            assert(WMFWUploadPlanner::generatePlan(i, region, {252, true, true}, *plan));
            uint32_t bytes = 0;
            for (uint32_t j = 0; j < plan->transactionCount; ++j) {
                const auto& transaction = plan->transactions[j];
                assert(transaction.size && transaction.size <= 252);
                assert(transaction.payload == region.data.begin + bytes);
                bytes += transaction.size;
            }
            assert(bytes == region.size);
            ++memoryRegions;
        }
    }
    assert(memoryRegions == 27);
    MemoryIO memory;
    auto session = std::make_unique<UploadSession>();
    assert(WMFWUploadScheduler::run("cs35l56-test", memory, *mapped, *session));
    assert(session->complete && session->regionCount == 27 && session->passCount == 27);
    const unsigned totalWrites = memory.writes;
    for (unsigned failure = 1; failure <= totalWrites; ++failure) {
        MemoryIO failing;
        failing.failWrite = failure;
        assert(!WMFWUploadScheduler::run("cs35l56-write-fault", failing, *mapped, *session));
        assert(!session->complete && session->passCount < 27);
        assert(session->regionCount == session->passCount + 1);
    }
    for (unsigned failure = 1; failure <= memory.reads; ++failure) {
        MemoryIO failing;
        failing.failRead = failure;
        assert(!WMFWUploadScheduler::run("cs35l56-read-fault", failing, *mapped, *session));
        assert(!session->complete && session->passCount < 27);
    }
    std::vector<uint8_t> packedWords;
    const auto& first = mapped->regions[1];
    assert(first.regionType == RegionType::XM_PACKED && first.firmwareAddress == 0);
    for (uint32_t i = 0; i < first.size; i += 4)
        for (int byte = 3; byte >= 0; --byte)
            packedWords.push_back(first.data.begin[i + byte]);
    std::vector<uint8_t> algorithmTable;
    for (size_t i = 0; i + 3 <= packedWords.size(); i += 3) {
        algorithmTable.push_back(0);
        algorithmTable.push_back(packedWords[i + 2]);
        algorithmTable.push_back(packedWords[i + 1]);
        algorithmTable.push_back(packedWords[i]);
    }
    assert(WMFWParser::parseAlgorithmTable(algorithmTable.data(), algorithmTable.size(), *image));
    assert(image->firmwareId == 0x1a00d6 && image->haloFirmwareVersion == 0x030b10 && image->algorithmCount == 42);
    // Exercise the production live-table reader, not just the parser fed from
    // the file. Packed download bytes are not the unpacked register view.
    MemoryIO live;
    for (size_t i = 0; i < algorithmTable.size(); ++i)
        live.memory[registers::kRegDspXMemory + uint32_t(i)] = algorithmTable[i];
    assert(DSPControl::readAlgorithms(live, *image, 0x1a00d6, 0x030b10));
    assert(image->algorithmCount == 42 && image->firmwareId == 0x1a00d6);
    const unsigned tableReads = live.reads;
    for (unsigned failure = 1; failure <= tableReads; ++failure) {
        MemoryIO failing;
        failing.memory = live.memory;
        failing.failRead = failure;
        assert(!DSPControl::readAlgorithms(failing, *image, 0x1a00d6, 0x030b10));
        assert(image->algorithmCount == 0 && image->algorithmTotal == 0 && image->firmwareId == 0);
        assert(failing.reads == failure && !failing.writes);
    }
    for (auto expected : {0x1a00d7U, 0U}) {
        assert(!DSPControl::readAlgorithms(live, *image, expected, 0x030b10));
        assert(image->algorithmCount == 0);
    }
    assert(!DSPControl::readAlgorithms(live, *image, 0x1a00d6, 0x030b11));
    assert(image->algorithmCount == 0);
    assert(DSPControl::readAlgorithms(live, *image, 0x1a00d6, 0x030b10));
    for (uint32_t count : {0U, 64U, 0xffffffffU}) {
        MemoryIO badCount;
        badCount.memory = live.memory;
        for (unsigned byte = 0; byte < 4; ++byte)
            badCount.memory[registers::kRegDspXMemory + 36 + byte] = uint8_t(count >> (24 - byte * 8));
        assert(!DSPControl::readAlgorithms(badCount, *image, 0x1a00d6, 0x030b10));
        assert(image->algorithmCount == 0 && badCount.reads == 1 && !badCount.writes);
    }
    assert(DSPControl::readAlgorithms(live, *image, 0x1a00d6, 0x030b10));
    for (bool right : {false, true}) {
        assert(WMFWParser::parseBIN(right ? resource->binRight : resource->bin,
                                  right ? resource->binRightSize : resource->binSize, image.get()));
        auto coefficients = std::make_unique<MappedImage>();
        assert(WMFWMemoryMapper::mapCoefficients(*image, *coefficients));
        assert(coefficients->regionCount == 39);
        for (uint32_t i = 0; i < coefficients->regionCount; ++i) {
            const auto& region = coefficients->regions[i];
            uint32_t base = 0, limit = 0;
            switch (region.regionType) {
            case RegionType::XM_PACKED: base = 0x02000000; limit = 0x02006000; break;
            case RegionType::XM_UNPACKED: base = 0x02800000; limit = 0x02808000; break;
            case RegionType::YM_PACKED: base = 0x02c00000; limit = 0x02c047f4; break;
            case RegionType::YM_UNPACKED: base = 0x03400000; limit = 0x03405ff8; break;
            default: assert(false);
            }
            assert(region.dspRegister >= base && uint64_t(region.dspRegister) + region.size <= limit);
        }
        MemoryIO tuningMemory;
        assert(WMFWUploadScheduler::run("cs35l56-tuning", tuningMemory, *coefficients, *session));
        assert(session->complete && session->passCount == coefficients->regionCount);
        for (unsigned failure = 1; failure <= tuningMemory.writes; ++failure) {
            MemoryIO failing;
            failing.failWrite = failure;
            assert(!WMFWUploadScheduler::run("cs35l56-tuning-fault", failing, *coefficients, *session));
            assert(!session->complete && session->passCount < coefficients->regionCount);
        }
    }
    for (const char* name : {"CAL_CHECKSUM", "CAL_STATUS", "CAL_R", "CAL_AMBIENT"}) {
        WMFWControlRef control{};
        uint32_t address = 0;
        assert(WMFWParser::findControl(image.get(), name, control));
        assert(WMFWParser::resolveControl(*image, control, address));
        assert(address >= 0x03400000 && address <= 0x03405ff4);
    }
    image->algorithmCount = 0;
    const char* names[] = {"CAL_CHECKSUM", "CAL_STATUS", "CAL_R", "CAL_AMBIENT"};
    const uint16_t offsets[] = {3, 4, 5, 0x42};
    for (unsigned i = 0; i < 4; ++i) {
        WMFWControlRef control{};
        assert(WMFWParser::findControl(image.get(), names[i], control));
        assert(control.algorithm->id == 0x9f210);
        assert(control.control->type == WMFW_ADSP2_YM);
        assert(control.control->offset == offsets[i]);
        assert(control.control->len == 4);
        uint32_t address = 1;
        assert(!WMFWParser::resolveControl(*image, control, address));
        assert(address == 0);
    }
    assert(WMFWParser::parseBIN(resource->bin,resource->binSize,image.get()));
    assert(WMFWParser::parseBIN(resource->binRight,resource->binRightSize,image.get()));
    std::vector<uint8_t> boundary(resource->wmfw, resource->wmfw + 40);
    boundary.resize(40 + MAX_FIRMWARE_REGIONS * 8);
    for (size_t i = 40; i < boundary.size(); i += 8)
        boundary[i + 3] = WMFW_NAME_TEXT;
    assert(WMFWParser::parseWMFW(boundary.data(), boundary.size(), image.get()));
    assert(image->regionCount == MAX_FIRMWARE_REGIONS);
    boundary.resize(boundary.size() + 8);
    boundary[boundary.size() - 5] = WMFW_NAME_TEXT;
    assert(!WMFWParser::parseWMFW(boundary.data(), boundary.size(), image.get()));
    assert(image->regionCount == 0 && image->wmfwControlCount == 0);
    std::vector<uint8_t> table(40 + 63 * 24);
    auto putBE = [&](size_t word, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte)
            table[word * 4 + byte] = uint8_t(value >> (24 - byte * 8));
    };
    putBE(9, 63);
    putBE(3, 1);
    for (unsigned i = 0; i < 63; ++i)
        putBE(10 + i * 6, i + 2);
    assert(WMFWParser::parseAlgorithmTable(table.data(), table.size(), *image));
    assert(image->algorithmCount == 64);
    assert(!WMFWParser::parseAlgorithmTable(table.data(), table.size() - 1, *image));
    assert(image->algorithmCount == 0);
    putBE(9, 64);
    assert(!WMFWParser::parseAlgorithmTable(table.data(), table.size(), *image));
    assert(image->algorithmCount == 0);
}
''')
    executable=tmp/"test.exe"
    for flags in sanitizer_flags():
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"),
                        str(ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/Resources/FirmwareData.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print("PASS exact CS35L56 resources, distinct AMP coefficients, BIN parsing and complete WMFW metadata")
