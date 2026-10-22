// CS35L56 identity and diagnostic read state owned by each amplifier driver.
#pragma once
#include <IOKit/IOLib.h>
#include <stdint.h>

namespace cirrus::devices::amplifiers::cs35l56 {
struct InstanceState {
    uint32_t deviceId{0}, revisionId{0}, releaseId{0}, otpId{0}, failedRegister{0};
    IOReturn result{kIOReturnNotReady};
    uint8_t readMask{0};
    bool identified{false}, revisionSupported{false};
};
}
