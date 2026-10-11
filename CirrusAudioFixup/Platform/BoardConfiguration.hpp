// Transport-neutral board manifest for future backends. This does not infer
// reset, boost, ASP routing or tuning from a silicon ID or a subsystem vendor.
#pragma once
#include "Core/Endpoint.hpp"
namespace cirrus::platform {
struct BoardIdentity {
    uint16_t subsystemVendor{0};
    uint16_t subsystemDevice{0};
    const char* acpiHid{nullptr};
};
struct AmplifierBinding {
    core::BusEndpoint endpoint{};
    uint32_t siliconId{0};
    uint32_t channelIndex{0};
    uint32_t speakerId{0};
    const char* tuningResource{nullptr};
};
struct BoardConfiguration {
    BoardIdentity identity{};
    const AmplifierBinding* amplifiers{nullptr};
    size_t count{0};
    bool wiringVerified{false};
    bool powerVerified{false};
    bool tuningVerified{false};
    bool permits(const BoardIdentity& actual) const {
        if(!wiringVerified || !powerVerified || !tuningVerified || !identity.subsystemVendor ||
            !identity.subsystemDevice || !identity.acpiHid || !actual.acpiHid ||
            identity.subsystemVendor!=actual.subsystemVendor || identity.subsystemDevice!=actual.subsystemDevice ||
            strcmp(identity.acpiHid,actual.acpiHid) || !amplifiers || !count || count>16)return false;
        for(size_t i=0;i<count;++i) {
            const auto& a=amplifiers[i];
            if(!a.endpoint.valid() || !a.siliconId || !a.tuningResource || !*a.tuningResource)return false;
            for(size_t j=0;j<i;++j)if(a.endpoint.sameDevice(amplifiers[j].endpoint))return false;
        }
        return true;
    }
};
}
