// Silicon-owned electrical properties; neither bus nor laptop identity selects
// these settings. Legacy bindings retain their established configuration.
#pragma once
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
struct DeviceConfiguration {
    PowerConfiguration power{};
    uint8_t gpio1Function{0}, gpio2Function{2};
    bool valid() const {
        // VSPK switch and MDSYNC need additional sequencing, deliberately not
        // inferred from a recognized HID or a matching firmware resource.
        return power.valid() && gpio1Function == 0 && (gpio2Function == 0 || gpio2Function == 2);
    }
};
inline constexpr const char* kDeviceConfigurationKind = "cs35l41-device-v1";
inline bool decodeDeviceConfiguration(const void* data, size_t size, const char* kind, DeviceConfiguration& output) {
    if (kind && !strcmp(kind, kDeviceConfigurationKind)) {
        if (!data || size != sizeof(DeviceConfiguration))
            return false;
        const auto& value = *static_cast<const DeviceConfiguration*>(data);
        if (!value.valid())
            return false;
        output = value;
        return true;
    }
    const auto* power = powerConfiguration(data, size, kind);
    if (!power)
        return false;
    output = {};
    output.power = *power;
    return true;
}
}
