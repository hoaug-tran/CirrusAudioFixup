// Owned by CS35L41Driver; endpoint/register/resource views remain borrowed.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Core/BusEndpoint.hpp"
#include "Devices/Amplifiers/CS35L41/RuntimeState.hpp"
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {
struct InstanceState : cirrus::devices::amplifiers::cs35l41::RuntimeState {
    char endpointLabel[16]{};
    // Identity and channel come from the silicon probe and board endpoint.
    // Do not infer routing or tuning from a display name or slave address.
    const char* name{nullptr};
    uint8_t address{0};
    // Borrowed register port; framing and controller lifetime belong to platform.
    cirrus::core::RegisterIO* registerIO{nullptr};
    cirrus::core::BusEndpoint endpoint{};
    uint8_t endpointIndex{0};
    cirrus::core::AmplifierModel model{cirrus::core::AmplifierModel::Unknown};
    cirrus::core::AudioChannel channel{cirrus::core::AudioChannel::Left};
    cirrus::devices::amplifiers::cs35l41::PowerConfiguration power{};
    uint8_t gpio1Function{0}, gpio2Function{2};
    bool explicitGPIOConfiguration{false};
    uint32_t firmwareSubsystemId{0};
    uint32_t firmwareSpeakerId{UINT32_MAX};
};

}
