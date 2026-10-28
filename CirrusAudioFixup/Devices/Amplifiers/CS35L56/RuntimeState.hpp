#pragma once
#include "Core/AmplifierLifecycle.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
struct RuntimeState {
    bool authorized{false}, hardwareTouched{false}, booted{false}, firmwareMissing{false};
    bool firmwareValidated{false}, calibrated{false}, initialized{false}, faulted{false}, playbackActive{false};
    bool routePrepared{false};
    uint32_t firmwareVersion{0}, routeGeneration{0};
    IOReturn firstFailure{kIOReturnSuccess}, latestFailure{kIOReturnSuccess};
    core::BringUpDisposition bringUpDisposition{core::BringUpDisposition::Continue};
};
}
