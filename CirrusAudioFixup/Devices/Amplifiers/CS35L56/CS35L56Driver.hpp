// CS35L56 implementation and owned identity state.
// Lifecycle initialization, firmware and playback are not enabled yet.
#pragma once
#include "Devices/Amplifiers/CS35L56/InstanceState.hpp"
#include "Runtime/AmplifierDriverRegistry.hpp"
#include "Runtime/AmplifierHost.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56Driver final {
public:
    explicit CS35L56Driver(const platform::AmplifierDriverContext& context) : mContext(context), mHost(*context.host) {}
    void inspect();

private:
    const platform::AmplifierDriverContext& mContext;
    platform::AmplifierHost& mHost;
    InstanceState mAmplifier{};
    void probeAmplifier();
};
}
