// CS35L56 read-only silicon identification. The driver owns diagnostic state;
// no reset, wake, firmware or playback operations are enabled.
#pragma once
#include "Core/BusEndpoint.hpp"
#include "Core/RegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L56/InstanceState.hpp"
#include "Devices/Amplifiers/CS35L56/DSPControl.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
class CS35L56Device final {
public:
    CS35L56Device(const char* name, core::BusEndpoint endpoint) : mName(name), mEndpoint(endpoint) {}
    const char* name() const { return mName; }
    core::BusEndpoint endpoint() const { return mEndpoint; }
    const InstanceState& state() const { return mState; }
    static IOReturn prepareIdentity(core::RegisterIO& io) {
        uint32_t revision = 0;
        if (!io.read(registers::kRegRevisionId, &revision))
            return io.lastError() == kIOReturnSuccess ? kIOReturnError : io.lastError();
        if ((revision & registers::kRevisionIdMask) != registers::kRevisionB0)
            return kIOReturnUnsupported;
        // Linux reads REVID and waits for firmware boot before reading DEVID.
        if (DSPControl::waitForBoot(io, [](uint32_t us) { IOSleep((us + 999) / 1000); }))
            return kIOReturnSuccess;
        return io.lastError() == kIOReturnSuccess ? kIOReturnTimeout : io.lastError();
    }
    bool probe(core::RegisterIO& io) {
        mState = {};
        auto& result = mState;
        const uint32_t addresses[]{registers::kRegDeviceId, registers::kRegRevisionId, registers::kRegReleaseId, registers::kRegOtpId};
        uint32_t* values[]{&result.deviceId, &result.revisionId, &result.releaseId, &result.otpId};
        for (size_t i = 0; i < 4; ++i) {
            if (!io.readForDiagnostic(addresses[i], values[i])) {
                result.failedRegister = addresses[i];
                result.result = io.lastError() == kIOReturnSuccess ? kIOReturnError : io.lastError();
                return false;
            }
            result.readMask |= uint8_t(1U << i);
            if (i == 0) {
                result.identified = (result.deviceId & registers::kDeviceIdMask) == registers::kValDeviceId;
                if (!result.identified) {
                    result.result = kIOReturnNotFound;
                    return false;
                }
            }
            if (i == 1)
                result.revisionSupported = (result.revisionId & registers::kRevisionIdMask) == registers::kRevisionB0;
        }
        result.result = kIOReturnSuccess;
        return true;
    }

private:
    const char* mName;
    core::BusEndpoint mEndpoint;
    InstanceState mState{};
};
}
