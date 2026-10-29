// Own a SPI bus and register map; silicon modules supply framing/access policy.
#pragma once
#include "Core/RegisterMap.hpp"
#include "Transport/SPI/SPIRegisterBus.hpp"

namespace cirrus::transport {
class SPIRegisterIO : public core::RegisterIO {
public:
    static constexpr size_t kHeaderBytes = 6;
    static constexpr size_t kMaxPayloadBytes = 256;
    SPIRegisterIO(SPIController* controller, SPIEndpoint endpoint, const core::RegisterMapConfig& config,
                  bool allowWrites, core::RegisterOperationLock* lock)
        : mBus(controller, endpoint), mMap(mBus, config, allowWrites, lock) {}
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

protected:
    bool writeLockedSequence(const uint32_t* registers, const uint32_t* values, size_t count, uint32_t delayUs) {
        return mMap.writeSequence(registers, values, count, delayUs);
    }

private:
    SPIRegisterBus mBus;
    core::RegisterMap mMap;
};
}
