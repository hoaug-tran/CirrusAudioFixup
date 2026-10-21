#pragma once
// Register adapter and timing helpers shared by CS35L41 subsystems.
#include "Devices/Amplifiers/CS35L41/CS35L41Driver.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41Device.hpp"
#include "Devices/Amplifiers/CS35L41/DSPControl.hpp"
#include "Devices/Amplifiers/CS35L41/DeviceConfiguration.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/Tuning.hpp"
#include "Diagnostics/FailureNames.hpp"
#include "Support/Logging.hpp"
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

using namespace cirrus::diagnostics;
namespace cirrus::devices::amplifiers::cs35l41 {
inline uint64_t playbackTimeMilliseconds() {
    uint64_t nanos = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
    return nanos / 1000000;
}
class DriverRegisterIO final : public core::RegisterIO {
    CS35L41Driver* mDriver;
    CS35L41Driver::AmplifierState& mAmplifier;
    TraceSource mSource;

public:
    DriverRegisterIO(CS35L41Driver* driver, CS35L41Driver::AmplifierState& amp, TraceSource source = TRACE_PROBE)
        : mDriver(driver), mAmplifier(amp), mSource(source) {}
    int32_t lastError() const override { return mDriver->mLastTransferReturn; }
    bool read(uint32_t r, uint32_t* v) override { return mDriver->readRegister(mAmplifier, r, v, mSource); }
    bool write(uint32_t r, uint32_t v) override { return mDriver->writeRegister(mAmplifier, r, v, mSource); }
    bool updateBits(uint32_t r, uint32_t m, uint32_t v) override { return mDriver->updateRegisterBits(mAmplifier, r, m, v, mSource); }
    bool bulkRead(uint32_t r, uint8_t* p, size_t n) override { return mDriver->bulkRead(mAmplifier, r, p, n, TRACE_FIRMWARE); }
    bool bulkWrite(uint32_t r, const uint8_t* p, size_t n) override { return mDriver->bulkWrite(mAmplifier, r, p, n, TRACE_FIRMWARE); }
    bool pollBit(uint32_t r, uint32_t m, uint32_t v, uint32_t n) override {
        return mDriver->pollRegisterBit(mAmplifier, r, m, v, n, TRACE_PROBE);
    }
};

inline uint32_t calculateCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

inline uint32_t crc32Le(uint32_t crc, uint8_t const* buf, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return ~crc;
}

struct cs35l41_amp_cal_data {
    uint32_t calTarget[2];
    uint32_t calTime[2];
    int8_t calAmbient;
    uint8_t calStatus;
    uint16_t calR;
} __attribute__((packed));

struct cs35l41_amp_efi_data {
    uint32_t size;
    uint32_t count;
    cs35l41_amp_cal_data data[];
} __attribute__((packed));


}
