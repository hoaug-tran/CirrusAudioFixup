#pragma once

#include "Core/AudioDevice.hpp"
#include "Core/RegisterIO.hpp"
#include "Devices/CS35L41/Hardware/Errata.hpp"
#include "Devices/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"
#if defined(__APPLE__) && (defined(KERNEL) || defined(_KERNEL) || defined(__KERNEL__))
#include <IOKit/IOLib.h>
#else
inline void IODelay(unsigned) {}
#endif

namespace cirrus::devices::cs35l41 {

constexpr uint8_t kI2cAddressLeft = 0x40;
constexpr uint8_t kI2cAddressRight = 0x41;

class CS35L41Device final : public core::AudioDevice {
private:
    const char* mName{"unknown"};
    uint8_t mAddress{0};
    uint32_t mDeviceId{0};
    uint32_t mRevisionId{0};
    bool mPresent{false};
    bool mInitialized{false};
    bool mDspAlive{false};

public:
    CS35L41Device(const char* name, uint8_t address) : mName(name), mAddress(address) {}

    const char* name() const override { return mName; }
    uint8_t address() const override { return mAddress; }
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

    core::AudioDevice::InitResult initialize(core::RegisterIO& io) override {
        if (!mPresent)
            return core::AudioDevice::InitResult::DeviceNotFound;

        if (!io.write(registers::kRegSoftwareReset, registers::kValSoftwareReset))
            return core::AudioDevice::InitResult::ResetFailed;

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
            return core::AudioDevice::InitResult::ResetFailed;

        if (!io.pollBit(registers::kRegIrq1Status4, registers::kMaskOtpBootDone, registers::kMaskOtpBootDone, 100))
            return core::AudioDevice::InitResult::BootTimeout;

        uint32_t otpStatus = 0;
        if (!io.read(registers::kRegIrq1Status3, &otpStatus))
            return core::AudioDevice::InitResult::ResetFailed;

        if (otpStatus & registers::kMaskOtpBootError)
            return core::AudioDevice::InitResult::BootError;

        mInitialized = true;
        return core::AudioDevice::InitResult::Success;
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
        ok = io.write(registers::kRegAspTx3Source, 0x00000032) && ok;
        ok = io.write(registers::kRegAspTx4Source, 0x00000033) && ok;
        ok = io.write(registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = io.write(registers::kRegDsp1Rx2Source, 0x00000009) && ok;
        ok = io.write(registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = io.write(registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = io.write(registers::kRegDsp1Rx5Source, 0x00000020) && ok;
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
        const cs35l41_otp_map_element_t* otpMapMatch = nullptr;
        const cs35l41_otp_packed_element_t* otpMap;
        int bitOffset, wordOffset, i;
        uint32_t otpVal;
        uint32_t otpIdReg;
        uint32_t otpMem[80];

        if (!io.read(0x00000010, &otpIdReg))
            return false;

        for (size_t i = 0; i < support::arraySize(cs35l41_otp_map_map); i++) {
            if (cs35l41_otp_map_map[i].id == otpIdReg) {
                otpMapMatch = &cs35l41_otp_map_map[i];
                break;
            }
        }

        if (!otpMapMatch)
            return false;

        uint8_t otpRawBuf[80 * 4];
        if (!io.bulkRead(0x00000400, otpRawBuf, sizeof(otpRawBuf)))
            return false;

        for (int i = 0; i < 80; i++) {
            otpMem[i] = (otpRawBuf[i * 4] << 24) | (otpRawBuf[i * 4 + 1] << 16) | (otpRawBuf[i * 4 + 2] << 8) | (otpRawBuf[i * 4 + 3]);
        }

        otpMap = otpMapMatch->map;
        bitOffset = otpMapMatch->bit_offset;
        wordOffset = otpMapMatch->word_offset;

        for (i = 0; i < otpMapMatch->num_elements; i++) {
            if (bitOffset + otpMap[i].size - 1 >= 32) {
                otpVal = (otpMem[wordOffset] & support::genMask(31, bitOffset)) >> bitOffset;
                otpVal |= (otpMem[++wordOffset] & support::genMask(bitOffset + otpMap[i].size - 33, 0)) << (32 - bitOffset);
                bitOffset += otpMap[i].size - 32;
            } else if (bitOffset + otpMap[i].size - 1 >= 0) {
                otpVal = (otpMem[wordOffset] & support::genMask(bitOffset + otpMap[i].size - 1, bitOffset)) >> bitOffset;
                bitOffset += otpMap[i].size;
            } else {
                otpVal = 0;
            }

            if (bitOffset == 32) {
                bitOffset = 0;
                wordOffset++;
            }

            if (otpMap[i].reg != 0) {
                if (!io.updateBits(otpMap[i].reg, support::genMask(otpMap[i].shift + otpMap[i].size - 1, otpMap[i].shift),
                                   otpVal << otpMap[i].shift)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool applyErrata(core::RegisterIO& io) {
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

        if (patch) {
            for (size_t i = 0; i < count; i++) {
                io.write(patch[i].reg, patch[i].value);
            }
        }

        return lockTestKey(io);
    }

    bool setupGPIO(core::RegisterIO& io) {
        bool ok = true;
        ok = io.updateBits(registers::kRegGpio1Control1, 0x80001000, 0x80000000) && ok;
        ok = io.updateBits(registers::kRegGpio2Control1, 0x80001000, 0x80000000) && ok;
        ok = io.updateBits(registers::kRegGpioPadControl, 0x07000000, 0x02000000) && ok;
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

    bool bootDsp(core::RegisterIO& io, const uint8_t* wmfw, size_t wmfwSize, const uint8_t* bin, size_t binSize) override {
        (void)io;
        (void)wmfw;
        (void)wmfwSize;
        (void)bin;
        (void)binSize;
        return false;
    }

    bool startPlayback(core::RegisterIO& io) override {
        if (!mInitialized)
            return false;

        return io.updateBits(registers::kRegPowerControl2, 0x00000003, 0x00000000);
    }

    bool stopPlayback(core::RegisterIO& io) override { return io.updateBits(registers::kRegPowerControl2, 0x00000003, 0x00000003); }

    bool powerDown(core::RegisterIO& io) override {
        stopPlayback(io);
        mDspAlive = false;
        return true;
    }

    bool isAlive(core::RegisterIO& io) override {
        uint32_t val = 0;

        if (!io.read(registers::kRegDspMailbox1, &val))
            return false;
        return true;
    }
};

}
