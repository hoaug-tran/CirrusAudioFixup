#include "Devices/Amplifiers/CS35L56/CS35L56Device.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Driver.hpp"
#if !defined(KERNEL)
#include <stdio.h>
#endif

namespace cirrus::devices::amplifiers::cs35l56 {
void CS35L56Driver::probeAmplifier() {
    CS35L56Device device(mHost.name(), mContext.endpoint);
    device.probe(mContext.io);
    const auto runtime = static_cast<const RuntimeState&>(mAmplifier);
    mAmplifier = device.state();
    static_cast<RuntimeState&>(mAmplifier) = runtime;
}
void CS35L56Driver::inspect() {
    probeAmplifier();
    auto& host = mHost;
    const auto& result = mAmplifier;
    char key[80]{};
    auto publish = [&](const char* suffix, uint32_t value) {
        snprintf(key, sizeof(key), "Cirrus_CS35L56_%u_%s", unsigned(host.endpointIndex()), suffix);
        host.publish(key, value, 32);
    };
    host.publish("Cirrus_CS35L56_Access", "READ_ONLY_NO_WAKE_OR_RESET");
    publish("DeviceID", result.deviceId);
    publish("RevisionID", result.revisionId);
    publish("ReleaseID", result.releaseId);
    publish("OTPID", result.otpId);
    publish("Probe_Return", uint32_t(result.result));
    publish("Probe_Failed_Register", result.failedRegister);
    publish("Probe_Read_Mask", result.readMask);
    snprintf(key, sizeof(key), "Cirrus_CS35L56_%u_Probe_Status", unsigned(host.endpointIndex()));
    host.publish(key, result.result != kIOReturnSuccess ? "FAILED_NO_WAKE_ATTEMPT"
                      : result.revisionSupported        ? "IDENTIFIED_B0_READ_ONLY"
                                                        : "IDENTIFIED_UNVERIFIED_REVISION");
}
}
