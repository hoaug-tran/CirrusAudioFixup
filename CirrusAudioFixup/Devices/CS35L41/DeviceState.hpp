// Owned by CS35L41Driver; endpoint/register/resource views remain borrowed.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Core/BusEndpoint.hpp"
#include "Devices/CS35L41/State.hpp"

namespace cirrus::devices::cs35l41 {
struct DeviceState : cirrus::devices::cs35l41::State {
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
};

}
