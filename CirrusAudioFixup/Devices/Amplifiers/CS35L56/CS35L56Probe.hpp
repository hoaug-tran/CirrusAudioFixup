// Read-only control-port probe. Register facts: Linux 7be4bc52fd9e,
// include/sound/cs35l56.h and sound/soc/codecs/cs35l56-shared.c.
// No wake, reset, mailbox, interrupt acknowledgement or DSP writes.
#pragma once
#include "Core/RegisterMap.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
namespace registers {
inline constexpr uint32_t kDeviceId = 0x00000000, kRevisionId = 0x00000004;
inline constexpr uint32_t kReleaseId = 0x0000000c, kOtpId = 0x00000010;
inline constexpr uint32_t kDeviceIdMask = 0x00ffffff, kDeviceIdValue = 0x00035a56;
inline constexpr uint32_t kRevisionMask = 0x000000ff, kRevisionB0 = 0x000000b0;
}
constexpr bool readable(uint32_t reg) {
    return reg == registers::kDeviceId || reg == registers::kRevisionId || reg == registers::kReleaseId || reg == registers::kOtpId;
}
constexpr bool writable(uint32_t) { return false; }
constexpr core::RegisterMapConfig spiFormat() {
    core::RegisterMapConfig format{};
    format.paddingBytes = 2;
    // regmap-spi supplies 0x80 when regmap_config omits read_flag_mask.
    format.readFlag = 0x80;
    format.maxRegister = registers::kOtpId;
    format.readable = readable;
    format.writable = writable;
    return format;
}
struct ProbeResult {
    uint32_t deviceId{0}, revisionId{0}, releaseId{0}, otpId{0}, failedRegister{0};
    IOReturn result{kIOReturnNotReady};
    uint8_t readMask{0};
    bool identified{false}, revisionSupported{false};
};
inline ProbeResult probe(core::RegisterIO& io) {
    ProbeResult result;
    const uint32_t addresses[]{registers::kDeviceId, registers::kRevisionId, registers::kReleaseId, registers::kOtpId};
    uint32_t* values[]{&result.deviceId, &result.revisionId, &result.releaseId, &result.otpId};
    for (size_t i = 0; i < 4; ++i) {
        if (!io.readForDiagnostic(addresses[i], values[i])) {
            result.failedRegister = addresses[i];
            result.result = io.lastError() == kIOReturnSuccess ? kIOReturnError : io.lastError();
            return result;
        }
        result.readMask |= uint8_t(1U << i);
        if (i == 0) {
            result.identified = (result.deviceId & registers::kDeviceIdMask) == registers::kDeviceIdValue;
            if (!result.identified) {
                result.result = kIOReturnNotFound;
                return result;
            }
        }
        if (i == 1)
            result.revisionSupported = (result.revisionId & registers::kRevisionMask) == registers::kRevisionB0;
    }
    result.result = kIOReturnSuccess;
    return result;
}
}
