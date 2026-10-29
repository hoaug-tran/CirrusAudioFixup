// CS35L41's register format configured over the shared register-map engine.
// Linux cs35l41-lib.c: 32-bit BE address/value, 16 pad bits, read flag 0x80.
#pragma once
#include "Transport/SPI/SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
class CS35L41SPIRegisterIO final : public transport::SPIRegisterIO {
public:
    CS35L41SPIRegisterIO(transport::SPIController* controller, transport::SPIEndpoint endpoint, bool allowWrites = false,
                         core::RegisterOperationLock* lock = nullptr)
        : transport::SPIRegisterIO(controller, endpoint, config(), allowWrites, lock) {}
    static constexpr core::RegisterMapConfig config() {
        core::RegisterMapConfig c{};
        c.paddingBytes = 2;
        c.maxRegister = registers::kLastRegister;
        c.readFlag = 0x80;
        return c;
    }
};
}
