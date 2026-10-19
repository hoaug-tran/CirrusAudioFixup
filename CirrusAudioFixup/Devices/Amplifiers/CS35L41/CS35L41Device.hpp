//
// CS35L41Device.hpp
// CS35L41 identification, reset and register configuration.
// The independent CS35L41Driver owns firmware, playback and shutdown.
// Platform authorization remains with the borrowed host contract.
// See LICENSE for distribution terms.
//

#pragma once

#include "Core/AmplifierDevice.hpp"
#include "Core/RegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Errata.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"
#if defined(__APPLE__) && (defined(KERNEL) || defined(_KERNEL) || defined(__KERNEL__))
#include <IOKit/IOLib.h>
#else
inline void IODelay(unsigned) {}
#endif

namespace cirrus::devices::amplifiers::cs35l41 {

constexpr uint8_t kI2cAddressLeft = 0x40;
constexpr uint8_t kI2cAddressRight = 0x41;

class CS35L41Device final : public core::AmplifierDevice {
private:
    const char* mName{"unknown"};
    core::BusEndpoint mEndpoint{};
    uint32_t mDeviceId{0};
    uint32_t mRevisionId{0};
    bool mPresent{false};
    bool mInitialized{false};
    bool mDspAlive{false};

public:
    CS35L41Device(const char* name, uint8_t address) : mName(name) { mEndpoint.i2cAddress = address; }
    CS35L41Device(const char* name, core::BusEndpoint endpoint) : mName(name), mEndpoint(endpoint) {}

    const char* name() const override { return mName; }
    uint8_t address() const override {
        return mEndpoint.transport == core::BusTransportType::I2C && !mEndpoint.tenBitAddress && mEndpoint.i2cAddress <= 0x7f
                   ? uint8_t(mEndpoint.i2cAddress)
                   : 0xff;
    }
    core::BusEndpoint endpoint() const override { return mEndpoint; }
    uint32_t deviceId() const { return mDeviceId; }
    uint32_t revisionId() const { return mRevisionId; }

    const char* getName() const { return name(); }
    uint8_t getAddress() const { return address(); }
    uint32_t getDeviceId() const { return deviceId(); }
    uint32_t getRevisionId() const { return revisionId(); }

    bool isPresent() const { return mPresent; }
    bool isInitialized() const { return mInitialized; }
    bool isDspAlive() const { return mDspAlive; }

    bool probe(core::RegisterIO& io) override {
        uint32_t devid = 0;
        uint32_t revid = 0;
        if (!io.read(registers::kRegDeviceId, &devid) || !io.read(registers::kRegRevisionId, &revid)) {
            return false;
        }
        mDeviceId = devid;
        mRevisionId = revid;

        mPresent = (devid == registers::kValDeviceId);
        return mPresent;
    }

    core::AmplifierDevice::InitResult initialize(core::RegisterIO& io) override {
        if (!mPresent)
            return core::AmplifierDevice::InitResult::DeviceNotFound;

        if (!io.write(registers::kRegSoftwareReset, registers::kValSoftwareReset))
            return core::AmplifierDevice::InitResult::ResetFailed;

        IODelay(3000);

        bool irqMasksOk = true;
        irqMasksOk = io.write(registers::kRegIrq1Mask1, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq1Mask2, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq1Mask3, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq1Mask4, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq2Mask1, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq2Mask2, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq2Mask3, 0xFFFFFFFF) && irqMasksOk;
        irqMasksOk = io.write(registers::kRegIrq2Mask4, 0xFFFFFFFF) && irqMasksOk;
        if (!irqMasksOk)
            return core::AmplifierDevice::InitResult::ResetFailed;

        if (!io.pollBit(registers::kRegIrq1Status4, registers::kMaskOtpBootDone, registers::kMaskOtpBootDone, 100))
            return core::AmplifierDevice::InitResult::BootTimeout;

        uint32_t otpStatus = 0;
        if (!io.read(registers::kRegIrq1Status3, &otpStatus))
            return core::AmplifierDevice::InitResult::ResetFailed;

        if (otpStatus & registers::kMaskOtpBootError)
            return core::AmplifierDevice::InitResult::BootError;

        mInitialized = true;
        return core::AmplifierDevice::InitResult::Success;
    }

    bool setupPLL(core::RegisterIO& io, bool* locked) {
        bool ok = true;
        ok = io.write(registers::kRegPllClockControl, 0x00000430) && ok;
        ok = io.write(registers::kRegDspClockControl, 0x00000003) && ok;
        ok = io.write(registers::kRegGlobalClockControl, 0x00000003) && ok;
        if (!ok)
            return false;

        uint32_t sts = 0;
        if (!io.read(registers::kRegIrq1RawStatus3, &sts))
            return false;
        if (locked)
            *locked = (sts & 0x00000002) != 0;
        return true;
    }

    bool setupASP(core::RegisterIO& io, bool isRight, bool firmwareValidated) {
        bool ok = true;
        ok = io.write(registers::kRegSerialPortRateControl, 0x00000021) && ok;
        ok = io.write(registers::kRegSerialPortFormat, 0x20200200) && ok;
        ok = io.write(registers::kRegSerialPortTxWordLength, 0x00000018) && ok;
        ok = io.write(registers::kRegSerialPortRxWordLength, 0x00000018) && ok;
        ok = io.write(registers::kRegAmplifierGainControl, 0x00000000) && ok;
        ok = io.write(registers::kRegDacPcm1Source, 0x00000008) && ok;
        ok = io.write(registers::kRegAspTx1Source, 0x00000018) && ok;
        ok = io.write(registers::kRegAspTx2Source, 0x00000019) && ok;
        ok = io.write(registers::kRegAspTx3Source, 0) && ok;
        ok = io.write(registers::kRegAspTx4Source, 0) && ok;
        ok = io.write(registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = io.write(registers::kRegDsp1Rx2Source, 0x00000009) && ok;
        ok = io.write(registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = io.write(registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = io.write(registers::kRegDsp1Rx5Source, 0x00000020) && ok;
        ok = io.write(registers::kRegDsp1Rx6Source, 0x00000021) && ok;
        ok = io.write(registers::kRegSerialPortHighImpedanceControl, 0x00000002) && ok;
        ok = io.write(registers::kRegSerialPortEnables, 0x00010000) && ok;

        uint32_t rxSlotVal = 0;
        if (!io.read(registers::kRegSerialPortFrameRxSlot, &rxSlotVal))
            return false;
        rxSlotVal &= ~0x3F;
        rxSlotVal |= (isRight ? 1 : 0);
        ok = io.write(registers::kRegSerialPortFrameRxSlot, rxSlotVal) && ok;

        uint32_t dacPcm1Src = firmwareValidated ? 0x32 : 0x08;
        ok = io.write(registers::kRegDacPcm1Source, dacPcm1Src) && ok;
        return ok;
    }

    bool unlockTestKey(core::RegisterIO& io) {
        return io.write(registers::kRegTestKeyControl, 0x00000055) && io.write(registers::kRegTestKeyControl, 0x000000AA);
    }

    bool lockTestKey(core::RegisterIO& io) {
        return io.write(registers::kRegTestKeyControl, 0x000000CC) && io.write(registers::kRegTestKeyControl, 0x00000033);
    }

    bool unpackOTP(core::RegisterIO& io) {
        // OTP fields form a packed bitstream, not an array of trim values.
        // A field may consume two words; validate both indices before reading.
        const cs35l41_otp_map_element_t* otpMapMatch = nullptr;
        const cs35l41_otp_packed_element_t* otpMap;
        int bitOffset, wordOffset;
        uint32_t otpVal;
        uint32_t otpIdReg;
        uint32_t otpMem[80];

        if (!io.read(0x00000010, &otpIdReg))
            return false;

        for (size_t mapIdx = 0; mapIdx < support::arraySize(cs35l41_otp_map_map); mapIdx++) {
            if (cs35l41_otp_map_map[mapIdx].id == otpIdReg) {
                otpMapMatch = &cs35l41_otp_map_map[mapIdx];
                break;
            }
        }

        if (!otpMapMatch)
            return false;

        uint8_t otpRawBuf[80 * 4];
        if (!io.bulkRead(0x00000400, otpRawBuf, sizeof(otpRawBuf)))
            return false;

        for (size_t memIdx = 0; memIdx < 80; memIdx++) {
            otpMem[memIdx] = (otpRawBuf[memIdx * 4] << 24) | (otpRawBuf[memIdx * 4 + 1] << 16) | (otpRawBuf[memIdx * 4 + 2] << 8) |
                             (otpRawBuf[memIdx * 4 + 3]);
        }

        otpMap = otpMapMatch->map;
        bitOffset = otpMapMatch->bit_offset;
        wordOffset = otpMapMatch->word_offset;

        for (size_t elemIdx = 0; elemIdx < otpMapMatch->num_elements; elemIdx++) {
            if (wordOffset >= 80)
                return false;
            if (bitOffset + otpMap[elemIdx].size - 1 >= 32) {
                if (wordOffset + 1 >= 80)
                    return false;
                otpVal = (otpMem[wordOffset] & support::genMask(31, bitOffset)) >> bitOffset;
                otpVal |= (otpMem[++wordOffset] & support::genMask(bitOffset + otpMap[elemIdx].size - 33, 0)) << (32 - bitOffset);
                bitOffset += otpMap[elemIdx].size - 32;
            } else if (bitOffset + otpMap[elemIdx].size - 1 >= 0) {
                otpVal = (otpMem[wordOffset] & support::genMask(bitOffset + otpMap[elemIdx].size - 1, bitOffset)) >> bitOffset;
                bitOffset += otpMap[elemIdx].size;
            } else {
                otpVal = 0;
            }

            if (bitOffset == 32) {
                bitOffset = 0;
                wordOffset++;
            }

            if (otpMap[elemIdx].reg != 0) {
                if (!io.updateBits(otpMap[elemIdx].reg,
                                   support::genMask(otpMap[elemIdx].shift + otpMap[elemIdx].size - 1, otpMap[elemIdx].shift),
                                   otpVal << otpMap[elemIdx].shift)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool applyErrata(core::RegisterIO& io) {
        // Keep the protected-register window bounded to the patch sequence.
        // Relocking is required even if the first patch write fails.
        if (!unlockTestKey(io))
            return false;

        const ErrataPatch* patch = nullptr;
        size_t count = 0;
        uint32_t revOnly = mRevisionId & 0xFF;

        switch (revOnly) {
        case 0xB2:
            patch = kCs35l41RevB2ErrataPatch;
            count = sizeof(kCs35l41RevB2ErrataPatch) / sizeof(ErrataPatch);
            break;
        default:
            break;
        }

        bool success = true;
        if (patch) {
            for (size_t idx = 0; idx < count; idx++) {
                if (!io.write(patch[idx].reg, patch[idx].value)) {
                    success = false;
                    break;
                }
            }
        }

        bool lockOk = lockTestKey(io);
        return success && lockOk;
    }

    bool setupGPIO(core::RegisterIO& io, uint8_t gpio1Function = 0, uint8_t gpio2Function = 2, bool configureGPIO1 = false) {
        if (gpio1Function != 0 || (gpio2Function != 0 && gpio2Function != 2))
            return false;
        bool ok = true;
        ok = io.updateBits(registers::kRegGpio1Control1, 0x80001000, 0x80000000) && ok;
        ok = io.updateBits(registers::kRegGpio2Control1, 0x80001000, 0x80000000) && ok;
        if (configureGPIO1)
            ok = io.updateBits(registers::kRegGpioPadControl, 0x00030000, uint32_t(gpio1Function) << 16) && ok;
        ok = io.updateBits(registers::kRegGpioPadControl, 0x07000000, uint32_t(gpio2Function) << 24) && ok;
        return ok;
    }

    bool configureHardware(core::RegisterIO& io) {
        bool ok = true;
        ok = io.write(registers::kRegIrq1Mask1, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq1Mask2, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq1Mask3, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq1Mask4, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq2Mask1, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq2Mask2, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq2Mask3, 0xFFFFFFFF) && ok;
        ok = io.write(registers::kRegIrq2Mask4, 0xFFFFFFFF) && ok;
        ok = io.write(0x00002020, 0x00000006) && ok;
        return ok;
    }

    bool startPlayback(core::RegisterIO&) override {
        // This helper lacks route, topology, firmware and pair readiness.
        // Only CS35L41Driver's verified lifecycle may authorize playback.
        return false;
    }

    bool stopPlayback(core::RegisterIO& io) override {
        // Emergency mute only, not a substitute for topology-aware shutdown.
        // AMP_EN is bit 0; setting bits 0/1 here used to enable the amplifier.
        bool ok = io.write(registers::kRegAmplifierGainControl, 0);
        ok = io.write(registers::kRegAmplifierDigitalVolumeControl, 0xA678) && ok;
        return io.updateBits(registers::kRegPowerControl2, 1, 0) && ok;
    }

    bool powerDown(core::RegisterIO& io) override {
        const bool muted = stopPlayback(io);
        uint32_t global = 0, power = 0;
        const bool idle = io.read(registers::kRegPowerControl1, &global) && io.read(registers::kRegPowerControl2, &power) &&
                          !(global & 1) && !(power & (0x3001 | registers::kMaskBoostEnable));
        if (muted && idle)
            mDspAlive = false;
        return muted && idle;
    }

    bool isAlive(core::RegisterIO& io) override {
        uint32_t val = 0;

        return io.read(registers::kRegDeviceId, &val) && val == registers::kValDeviceId;
    }
};

}
