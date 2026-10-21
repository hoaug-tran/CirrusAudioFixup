// Board identity, endpoints and verified wiring shared by amplifier drivers.
// Silicon identity alone does not determine power, routing or speaker tuning.
#pragma once
#include "Core/BusEndpoint.hpp"
namespace cirrus::platform {
inline bool parseACPISubsystemId(const char* text, uint32_t& output) {
    if (!text || strlen(text) != 8)
        return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const char c = text[i];
        unsigned digit = c >= '0' && c <= '9'   ? unsigned(c - '0')
                         : c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10)
                         : c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10)
                                                : 16;
        if (digit > 15)
            return false;
        value = (value << 4) | digit;
    }
    if (!(value >> 16) || !(value & 0xffff) || value == 0xffffffffU)
        return false;
    output = value;
    return true;
}
struct BoardIdentity {
    uint16_t subsystemVendor{0};
    uint16_t subsystemDevice{0};
    const char* acpiHid{nullptr};
};
struct AmplifierBinding {
    core::BusEndpoint endpoint{};
    uint32_t siliconId{0};
    uint32_t channelIndex{0};
    uint32_t speakerId{UINT32_MAX};
    const char* tuningResource{nullptr};
    // Optional per-device HID for mixed-silicon boards; board HID is the legacy default.
    const char* acpiHid{nullptr};
    // Zero: no configured amplifier GPIO interrupt.
    uint8_t interruptGpio{0};
    // Immutable module-owned configuration. Only its driver interprets the
    // schema; the shared runtime must not learn silicon power/register fields.
    const void* driverConfiguration{nullptr};
    size_t driverConfigurationSize{0};
    const char* driverConfigurationKind{nullptr};
};
struct BoardConfiguration {
    BoardIdentity identity{};
    const AmplifierBinding* amplifiers{nullptr};
    size_t count{0};
    bool wiringVerified{false};
    bool powerVerified{false};
    bool tuningVerified{false};
    bool useBoardFirmwareIdentity{false};
    bool permits(const BoardIdentity& actual) const {
        if (!wiringVerified || !powerVerified || !tuningVerified || !identity.subsystemVendor || !identity.subsystemDevice ||
            identity.subsystemVendor != actual.subsystemVendor || identity.subsystemDevice != actual.subsystemDevice ||
            (identity.acpiHid && (!*identity.acpiHid || !actual.acpiHid || strcmp(identity.acpiHid, actual.acpiHid))) || !amplifiers ||
            !count || count > 16)
            return false;
        for (size_t i = 0; i < count; ++i) {
            const auto& a = amplifiers[i];
            if (!a.endpoint.valid() || !a.siliconId || !a.tuningResource || !*a.tuningResource || (a.acpiHid && !*a.acpiHid) ||
                (!identity.acpiHid && !a.acpiHid))
                return false;
            for (size_t j = 0; j < i; ++j)
                if (a.endpoint.sameDevice(amplifiers[j].endpoint))
                    return false;
        }
        return true;
    }
};
}
