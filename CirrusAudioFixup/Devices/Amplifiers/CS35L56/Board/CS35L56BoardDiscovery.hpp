// CS35L56 firmware properties and read-only speaker identity inspection.
// No writable board configuration is inferred from identity.
#pragma once
#include "Platform/Board/BoardDiscovery.hpp"

namespace cirrus::devices::amplifiers::cs35l56 {
inline platform::DiscoveredBoard* resolveBoardConfiguration(const platform::BoardDiscoveryContext& context, const char*& reason) {
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
}
