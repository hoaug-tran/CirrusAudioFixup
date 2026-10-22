// CS35L56 register addresses and identity bitfields.
// Linux 7be4bc52fd9e: include/sound/cs35l56.h and cs35l56-shared.c.
// See LICENSE for distribution terms.
#pragma once
#include <stdint.h>

namespace cirrus::devices::amplifiers::cs35l56::registers {
constexpr uint32_t kRegDeviceId = 0x00000000;
constexpr uint32_t kRegRevisionId = 0x00000004;
constexpr uint32_t kRegReleaseId = 0x0000000C;
constexpr uint32_t kRegOtpId = 0x00000010;
constexpr uint32_t kDeviceIdMask = 0x00FFFFFF;
constexpr uint32_t kValDeviceId = 0x00035A56;
constexpr uint32_t kRevisionIdMask = 0x000000FF;
constexpr uint32_t kRevisionB0 = 0x000000B0;
// Diagnostic map boundary; DSP and playback register access is not enabled.
constexpr uint32_t kLastRegister = kRegOtpId;
}
