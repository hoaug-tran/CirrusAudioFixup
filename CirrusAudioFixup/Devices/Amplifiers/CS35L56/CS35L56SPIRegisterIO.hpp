// CS35L56's register format configured over the shared register-map engine.
// Linux 7be4bc52fd9e cs35l56-shared.c/regmap-spi.c: BE address/value, 16 pad bits, read flag 0x80.
#pragma once
#include "Transport/SPI/SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56SPIRegisterIO final : public transport::SPIRegisterIO {
public:
    CS35L56SPIRegisterIO(transport::SPIController* controller, transport::SPIEndpoint endpoint, bool allowWrites = false,
                         core::RegisterOperationLock* lock = nullptr)
        : transport::SPIRegisterIO(controller, endpoint, config(), allowWrites, lock) {}
    bool writeSequence(const uint32_t* registers, const uint32_t* values, size_t count, uint32_t delayUs) override {
        return writeLockedSequence(registers, values, count, delayUs);
    }
    static constexpr bool writableRegister(uint32_t reg) {
        using namespace registers;
        return dspMemoryRegister(reg) || reg == kRegAspEnables || reg == kRegAspControl1 || reg == kRegAspControl2 ||
               reg == kRegAspControl3 || reg == kRegAspFrame1 || reg == kRegAspFrame5 || reg == kRegAspData1 || reg == kRegAspData5 ||
               (reg >= kRegAspTx1 && reg <= kRegAspTx4) || (reg >= kRegSoundWireInput1 && reg <= kRegSoundWireInput4) ||
               reg == kRegIrqMask18 || reg == kRegIrqMask20 || reg == kRegDspRawMailbox1 || reg == kRegDspVirtualMailbox1;
    }
    static constexpr bool readableRegister(uint32_t reg) {
        using namespace registers;
        return writableRegister(reg) || reg == kRegDeviceId || reg == kRegRevisionId || reg == kRegReleaseId || reg == kRegOtpId ||
               reg == kRegIrqStatus || reg == kRegRestricted || (reg >= kRegOtpMemory53 && reg <= kRegOtpMemory53 + 8) ||
               reg == kRegDspVirtualMailbox2 || reg == kRegIrqFault1 || reg == kRegIrqFault8;
    }
    static constexpr core::RegisterMapConfig config() {
        core::RegisterMapConfig c{};
        c.paddingBytes = 2;
        c.maxRegister = registers::kLastRegister;
        c.readFlag = 0x80;
        c.readable = readableRegister;
        c.writable = writableRegister;
        return c;
    }
};
}
