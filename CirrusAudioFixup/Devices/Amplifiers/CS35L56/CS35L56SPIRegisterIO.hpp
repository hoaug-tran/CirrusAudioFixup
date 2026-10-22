// CS35L56's register format configured over the shared register-map engine.
// Linux 7be4bc52fd9e cs35l56-shared.c/regmap-spi.c: BE address/value, 16 pad bits, read flag 0x80.
#pragma once
#include "Core/RegisterMap.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
#include "Transport/SPI/SPIRegisterBus.hpp"
namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56SPIRegisterIO final : public core::RegisterIO {
public:
    static constexpr size_t kHeaderBytes = 6;
    static constexpr size_t kMaxPayloadBytes = 256;
    CS35L56SPIRegisterIO(transport::SPIController* controller, transport::SPIEndpoint endpoint, bool allowWrites = false,
                         core::RegisterOperationLock* lock = nullptr)
        : mBus(controller, endpoint), mMap(mBus, config(), allowWrites, lock) {}
    IOReturn lastReturn() const { return mMap.lastReturn(); }
    int32_t lastError() const override { return mMap.lastError(); }
    bool read(uint32_t reg, uint32_t* value) override { return mMap.read(reg, value); }
    bool write(uint32_t reg, uint32_t value) override { return mMap.write(reg, value); }
    bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) override { return mMap.updateBits(reg, mask, value); }
    bool bulkRead(uint32_t reg, uint8_t* data, size_t bytes) override { return mMap.bulkRead(reg, data, bytes); }
    bool bulkWrite(uint32_t reg, const uint8_t* data, size_t bytes) override { return mMap.bulkWrite(reg, data, bytes); }
    bool pollBit(uint32_t reg, uint32_t mask, uint32_t value, uint32_t attempts) override {
        return mMap.pollBit(reg, mask, value, attempts);
    }

public:
    static constexpr bool readableRegister(uint32_t reg) {
        return reg == registers::kRegDeviceId || reg == registers::kRegRevisionId || reg == registers::kRegReleaseId ||
               reg == registers::kRegOtpId;
    }
    static constexpr bool writableRegister(uint32_t) { return false; }
    static constexpr core::RegisterMapConfig config() {
        core::RegisterMapConfig c{};
        c.paddingBytes = 2;
        c.maxRegister = registers::kLastRegister;
        c.readFlag = 0x80;
        c.readable = readableRegister;
        c.writable = writableRegister;
        return c;
    }

private:
    transport::SPIRegisterBus mBus;
    core::RegisterMap mMap;
};
}
