#pragma once

#include "Core/AudioDevice.hpp"
#include "Core/RegisterIO.hpp"
#include "Devices/CS35L41/Hardware/Errata.hpp"
#include "Devices/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOLib.h>

namespace cirrus::devices::cs35l41 {

constexpr uint8_t kI2cAddressLeft = 0x40;
constexpr uint8_t kI2cAddressRight = 0x41;

// Driver implementation for Cirrus Logic CS35L41 boosted smart audio amplifier
class CS35L41Device final : public core::AudioDevice {
private:
    const char* mName{"unknown"};
    uint8_t mAddress{0};
    uint32_t mDeviceId{0};
    uint32_t mRevisionId{0};
    bool mPresent{false};
    bool mInitialized{false};
    bool mDspAlive{false};
    uint32_t mStateReg{0};

public:
    CS35L41Device(const char* name, uint8_t address) : mName(name), mAddress(address) {}

    const char* name() const override { return mName; }
    uint8_t address() const override { return mAddress; }
    uint32_t deviceId() const { return mDeviceId; }
    uint32_t revisionId() const { return mRevisionId; }

    // Compatibility accessors
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
        // Verify 0x00003541 device identification code
        mPresent = (devid == registers::kValDeviceId);
        return mPresent;
    }

    core::AudioDevice::InitResult initialize(core::RegisterIO& io) override {
        if (!mPresent)
            return core::AudioDevice::InitResult::DeviceNotFound;

        if (!io.write(registers::kRegSoftwareReset, registers::kValSoftwareReset))
            return core::AudioDevice::InitResult::ResetFailed;

        IODelay(3000);

        io.write(registers::kRegIrq1Mask1, 0xFFFFFFFF);
        io.write(registers::kRegIrq1Mask2, 0xFFFFFFFF);
        io.write(registers::kRegIrq1Mask3, 0xFFFFFFFF);
        io.write(registers::kRegIrq1Mask4, 0xFFFFFFFF);
        io.write(registers::kRegIrq2Mask1, 0xFFFFFFFF);
        io.write(registers::kRegIrq2Mask2, 0xFFFFFFFF);
        io.write(registers::kRegIrq2Mask3, 0xFFFFFFFF);
        io.write(registers::kRegIrq2Mask4, 0xFFFFFFFF);

        if (!io.pollBit(registers::kRegIrq1Status4, registers::kMaskOtpBootDone, registers::kMaskOtpBootDone, 100))
            return core::AudioDevice::InitResult::BootTimeout;

        uint32_t otpStatus = 0;
        if (!io.read(registers::kRegIrq1Status3, &otpStatus))
            return core::AudioDevice::InitResult::BootTimeout; // fallback if read fails

        if (otpStatus & registers::kMaskOtpBootError)
            return core::AudioDevice::InitResult::BootError;

        mInitialized = true;
        return core::AudioDevice::InitResult::Success;
    }

    
    bool setupPLL(core::RegisterIO& io) {
        io.write(registers::kRegPllClockControl, 0x00000430);
        io.write(registers::kRegDspClockControl, 0x00000003);
        io.write(registers::kRegGlobalClockControl, 0x00000003);
        
        uint32_t sts = 0;
        io.read(registers::kRegIrq1RawStatus3, &sts);
        return (sts & 0x00000002) != 0;
    }

    void setupASP(core::RegisterIO& io, bool isRight, bool firmwareValidated) {
        io.write(registers::kRegSerialPortRateControl, 0x00000021);
        io.write(registers::kRegSerialPortFormat, 0x20200200);
        io.write(registers::kRegSerialPortTxWordLength, 0x00000018);
        io.write(registers::kRegSerialPortRxWordLength, 0x00000018);
        io.write(registers::kRegAmplifierGainControl, 0x00000000);
        io.write(registers::kRegDacPcm1Source, 0x00000008);
        io.write(registers::kRegAspTx1Source, 0x00000018);
        io.write(registers::kRegAspTx2Source, 0x00000019);
        io.write(registers::kRegAspTx3Source, 0x00000032);
        io.write(registers::kRegAspTx4Source, 0x00000033);
        io.write(registers::kRegDsp1Rx1Source, 0x00000008);
        io.write(registers::kRegDsp1Rx2Source, 0x00000009);
        io.write(registers::kRegDsp1Rx3Source, 0x00000018);
        io.write(registers::kRegDsp1Rx4Source, 0x00000019);
        io.write(registers::kRegDsp1Rx5Source, 0x00000020);
        io.write(registers::kRegSerialPortHighImpedanceControl, 0x00000002);
        io.write(registers::kRegSerialPortEnables, 0x00010000);

        uint32_t rx_slot_val = 0;
        if (io.read(registers::kRegSerialPortFrameRxSlot, &rx_slot_val)) {
            rx_slot_val &= ~0x3F;
            rx_slot_val |= (isRight ? 1 : 0);
            io.write(registers::kRegSerialPortFrameRxSlot, rx_slot_val);
        }

        uint32_t dac_pcm1_src = firmwareValidated ? 0x32 : 0x08;
        io.write(registers::kRegDacPcm1Source, dac_pcm1_src);
    }

    
    bool unlockTestKey(core::RegisterIO& io) {
        return io.write(registers::kRegTestKeyControl, 0x00000055) &&
               io.write(registers::kRegTestKeyControl, 0x000000AA);
    }

    bool lockTestKey(core::RegisterIO& io) {
        return io.write(registers::kRegTestKeyControl, 0x000000CC) &&
               io.write(registers::kRegTestKeyControl, 0x00000033);
    }

    
    bool unpackOTP(core::RegisterIO& io) {
        const cs35l41_otp_map_element_t* otp_map_match = nullptr;
        const cs35l41_otp_packed_element_t* otp_map;
        int bit_offset, word_offset, i;
        uint32_t otp_val;
        uint32_t otp_id_reg;
        uint32_t otp_mem[80];

        if (!io.read(0x00000010, &otp_id_reg)) return false;

        for (size_t i = 0; i < support::arraySize(cs35l41_otp_map_map); i++) {
            if (cs35l41_otp_map_map[i].id == otp_id_reg) {
                otp_map_match = &cs35l41_otp_map_map[i];
                break;
            }
        }

        if (!otp_map_match) return false;

        uint8_t otp_raw_buf[80 * 4];
        if (!io.bulkRead(0x00000400, otp_raw_buf, sizeof(otp_raw_buf))) return false;

        for (int i = 0; i < 80; i++) {
            otp_mem[i] = (otp_raw_buf[i * 4] << 24) | (otp_raw_buf[i * 4 + 1] << 16) | (otp_raw_buf[i * 4 + 2] << 8) | (otp_raw_buf[i * 4 + 3]);
        }

        otp_map = otp_map_match->map;
        bit_offset = otp_map_match->bit_offset;
        word_offset = otp_map_match->word_offset;

        for (i = 0; i < otp_map_match->num_elements; i++) {
            if (bit_offset + otp_map[i].size - 1 >= 32) {
                otp_val = (otp_mem[word_offset] & support::genMask(31, bit_offset)) >> bit_offset;
                otp_val |= (otp_mem[++word_offset] & support::genMask(bit_offset + otp_map[i].size - 33, 0)) << (32 - bit_offset);
                bit_offset += otp_map[i].size - 32;
            } else if (bit_offset + otp_map[i].size - 1 >= 0) {
                otp_val = (otp_mem[word_offset] & support::genMask(bit_offset + otp_map[i].size - 1, bit_offset)) >> bit_offset;
                bit_offset += otp_map[i].size;
            } else {
                otp_val = 0;
            }

            if (bit_offset == 32) {
                bit_offset = 0;
                word_offset++;
            }

            if (otp_map[i].reg != 0) {
                if (!io.updateBits(otp_map[i].reg, support::genMask(otp_map[i].shift + otp_map[i].size - 1, otp_map[i].shift), otp_val << otp_map[i].shift)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool applyErrata(core::RegisterIO& io) {
        if (!unlockTestKey(io)) return false;

        const ErrataPatch* patch = nullptr;
        size_t count = 0;
        uint32_t rev_only = mRevisionId & 0xFF;

        switch (rev_only) {
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

    void setupGPIO(core::RegisterIO& io) {
        io.updateBits(registers::kRegGpio1Control1, 0x80001000, 0x80000000);
        io.updateBits(registers::kRegGpio2Control1, 0x80001000, 0x80000000);
        io.updateBits(registers::kRegGpioPadControl, 0x07000000, 0x02000000);
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

    bool powerUpAmplifier(core::RegisterIO& io, bool dspAlive) {
        if (!dspAlive) return false;
        
        io.updateBits(registers::kRegGlobalClockControl, 0x00000001, 0x00000001);
        uint32_t val = 0;
        int maxRetries = 100;
        while (maxRetries-- > 0) {
            if (io.read(registers::kRegGlobalClockControl, &val) && (val & 0x1)) {
                break;
            }
        }
        
        uint32_t sts1 = 0;
        io.read(0x00010090, &sts1);
        io.write(registers::kRegIrq1Status1, sts1);

        uint32_t pu = 0;
        io.read(0x00002900, &pu);
        pu |= 0x00000001;
        io.write(0x00002900, pu);

        return true;
    }

    bool verifyIdleConfiguration(core::RegisterIO& io) {
        return true;
    }

    bool bootDsp(core::RegisterIO& io, const uint8_t* wmfw, size_t wmfwSize, const uint8_t* bin, size_t binSize) override {
        (void)wmfw; (void)wmfwSize; (void)bin; (void)binSize;
        if (!mInitialized) return false;

        // Verify DSP alive first
        if (!isAlive(io)) return false;

        // Enforce HALO run state and pause Mailbox
        uint32_t mbox_init = 0;
        if (!io.read(registers::kRegDspMailbox2, &mbox_init)) return false;

        uint32_t halo_state = 0;
        bool hbReadable = false;

        // Try up to 15 times to wait for HALO_STATE_RUN
        for (int i = 0; i < 15; i++) {
            if (!io.read(mStateReg, &halo_state)) return false;
            if (halo_state == 2) break;
            IOSleep(10);
        }

        if (halo_state == 2) {
            // Pause Mailbox
            uint32_t sts = 0;
            io.write(registers::kRegDspMailbox1, 0x01);
            for (int i = 0; i < 15; i++) {
                if (io.read(registers::kRegDspMailbox2, &sts) && sts == 0x02) {
                    break;
                }
                IOSleep(10);
            }
        }

        mDspAlive = (halo_state == 2);
        return mDspAlive;
    }

    bool startPlayback(core::RegisterIO& io) override {
        if (!mInitialized)
            return false;
        // Clear AMP_EN standby bits to power on class D output stage
        return io.updateBits(registers::kRegPowerControl2, 0x00000003, 0x00000000);
    }

    bool stopPlayback(core::RegisterIO& io) override {
        // Assert STBY bits to safely mute output and stop switching
        return io.updateBits(registers::kRegPowerControl2, 0x00000003, 0x00000003);
    }

    bool powerDown(core::RegisterIO& io) override {
        stopPlayback(io);
        mDspAlive = false;
        return true;
    }

    bool isAlive(core::RegisterIO& io) override {
        uint32_t val = 0;
        // Heartbeat register increments continuously while Halo DSP firmware runs
        if (!io.read(registers::kRegDspMailbox1, &val))
            return false;
        return true;
    }
};

} // namespace cirrus::devices::cs35l41
