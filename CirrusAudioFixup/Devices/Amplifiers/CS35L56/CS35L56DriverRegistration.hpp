// SPI diagnostics only. A registered identity is not playback authorization.
#pragma once
#include "Devices/Amplifiers/CS35L56/CS35L56Probe.hpp"
#include "Runtime/AmplifierDriverRegistry.hpp"
#include "Runtime/AmplifierHost.hpp"
#if !defined(KERNEL)
#include <stdio.h>
#endif

namespace cirrus::devices::amplifiers::cs35l56 {
inline bool validDebugPhase(const char* phase) { return phase && !strcmp(phase, "probe"); }
inline platform::DiscoveredBoard* inspectBoard(const platform::BoardDiscoveryContext& context, const char*& reason) {
    reason = "CS35L56_DIAGNOSTIC_ONLY";
    if (!context.gpio || !context.properties.contains("spk-id-gpios"))
        return nullptr;
    uint32_t speaker = 0, next = 0;
    for (size_t i = 0; i < context.count; ++i) {
        if (!context.gpio->read("spk-id-gpios", i, next) || next > 1 || (i && next != speaker)) {
            reason = "CS35L56_SPEAKER_ID_UNAVAILABLE_OR_INCONSISTENT";
            return nullptr;
        }
        speaker = next;
    }
    if (context.count)
        reason = speaker ? "CS35L56_READ_ONLY_SPKID1" : "CS35L56_READ_ONLY_SPKID0";
    // Reading identity never creates a writable board or assumes tuning exists.
    return nullptr;
}
inline void inspectDevice(const platform::AmplifierDriverContext& context) {
    if (!context.host)
        return;
    auto& host = *context.host;
    const auto result = probe(context.io);
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
inline constexpr platform::AmplifierDriverRegistration kRegistration{"cs35l56-csc3556-probe",
                                                                     "CSC3556",
                                                                     registers::kDeviceIdValue,
                                                                     2,
                                                                     {},
                                                                     spiFormat(),
                                                                     nullptr,
                                                                     nullptr,
                                                                     false,
                                                                     registers::kDeviceId,
                                                                     registers::kDeviceIdMask,
                                                                     core::AmplifierModel::CS35L56,
                                                                     nullptr,
                                                                     inspectDevice,
                                                                     validDebugPhase,
                                                                     inspectBoard,
                                                                     nullptr,
                                                                     nullptr,
                                                                     true};
inline bool registerDrivers(platform::AmplifierDriverRegistry& registry) { return registry.add(kRegistration); }
}
