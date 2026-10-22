#pragma once

#include "Core/RegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
class DSPControl {
public:
    static bool readSiliconUid(core::RegisterIO& io, uint64_t& uid) {
        uid = 0;
        uint8_t pte[12]{};
        if (!io.bulkRead(registers::kRegOtpMemory53, pte, sizeof(pte)))
            return false;
        const uint32_t lot = uint32_t(pte[6]) | uint32_t(pte[5]) << 8 | uint32_t(pte[4]) << 16;
        const uint32_t die = uint32_t(pte[0]) | uint32_t(pte[7]) << 8 | uint32_t(pte[1]) << 16 | uint32_t(pte[11]) << 24;
        uid = (uint64_t(lot) << 32) | die;
        return true;
    }
    static bool readAlgorithms(core::RegisterIO& io, FirmwareImage& image, uint32_t firmwareId, uint32_t firmwareVersion) {
        image.algorithmCount = image.algorithmTotal = image.firmwareId = image.haloFirmwareVersion = 0;
        uint8_t table[40 + 63 * 24]{};
        if (!io.bulkRead(registers::kRegDspXMemory, table, 40))
            return false;
        const uint32_t count = WMFWParser::readUnpacked32BE(table, 9);
        if (!count || count > 63 || WMFWParser::readUnpacked32BE(table, 3) != firmwareId ||
            WMFWParser::readUnpacked32BE(table, 4) != firmwareVersion)
            return false;
        const size_t bytes = 40 + size_t(count) * 24;
        for (size_t offset = 40; offset < bytes;) {
            const size_t remaining = bytes - offset;
            const size_t chunk = remaining < 252 ? remaining : 252;
            if (!io.bulkRead(registers::kRegDspXMemory + uint32_t(offset), table + offset, chunk))
                return false;
            offset += chunk;
        }
        return WMFWParser::parseAlgorithmTable(table, bytes, image);
    }
    static bool waitForIdle(core::RegisterIO& io, void (*delayUs)(uint32_t)) {
        return poll(io, registers::kRegTransducerState, 3, 0xFFFE, registers::kPS3TimeoutUs, registers::kPS3PollUs, delayUs);
    }
    static bool waitForBoot(core::RegisterIO& io, void (*delayUs)(uint32_t)) {
        return poll(io, registers::kRegDspHaloState, 2, 0xFFFE, registers::kHaloStateTimeoutUs, registers::kHaloStatePollUs, delayUs);
    }
    static bool resetSPI(core::RegisterIO& io, void (*delayUs)(uint32_t)) {
        if (!delayUs)
            return false;
        const uint32_t addresses[]{registers::kRegDspVirtualMailbox1, registers::kRegDspRawMailbox1};
        const uint32_t values[]{registers::kCommandSystemReset, registers::kCommandPing};
        return io.writeSequence(addresses, values, 2, registers::kSPIResetToPortReadyUs) &&
               poll(io, registers::kRegDspVirtualMailbox2, 1, 0xFFFFFFFE, registers::kHaloStateTimeoutUs, registers::kHaloStatePollUs,
                    delayUs) &&
               waitForBoot(io, delayUs);
    }
    static bool sendMailbox(core::RegisterIO& io, uint32_t command, void (*delayUs)(uint32_t)) {
        return delayUs && io.write(registers::kRegDspVirtualMailbox1, command) &&
               poll(io, registers::kRegDspVirtualMailbox1, 0, 0, registers::kMailboxTimeoutUs, registers::kMailboxPollUs, delayUs);
    }
    static bool shutdown(core::RegisterIO& io, void (*delayUs)(uint32_t)) {
        return sendMailbox(io, registers::kCommandShutdown, delayUs) &&
               poll(io, registers::kRegDspPowerState, 1, 1, registers::kHaloStateTimeoutUs, registers::kHaloStatePollUs, delayUs);
    }

private:
    static bool poll(core::RegisterIO& io, uint32_t reg, uint32_t minimum, uint32_t maximum, uint32_t timeoutUs, uint32_t intervalUs,
                     void (*delayUs)(uint32_t)) {
        if (!delayUs)
            return false;
        for (uint32_t elapsed = 0;; elapsed += intervalUs) {
            uint32_t value = 0;
            if (!io.read(reg, &value))
                return false;
            if (value >= minimum && value <= maximum)
                return true;
            if (elapsed == timeoutUs)
                return false;
            delayUs(intervalUs);
        }
    }
};
}
