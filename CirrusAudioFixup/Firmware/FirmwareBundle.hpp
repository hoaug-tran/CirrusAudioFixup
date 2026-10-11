// Borrowed immutable firmware assets. Selection identity is separate from
// format parsing, upload and measured per-speaker calibration in each backend.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace cirrus::firmware {
enum class FirmwareFormat { None, WMFW, Vendor, Deferred };
struct FirmwareTarget {
    uint32_t siliconId{0};
    uint16_t subsystemVendor{0},subsystemDevice{0};
    uint32_t speakerId{0},channelIndex{0};
    const char* resourceId{nullptr};
    bool matches(const FirmwareTarget& other) const {
        return siliconId && subsystemVendor && subsystemDevice && resourceId && *resourceId &&
            other.resourceId && siliconId==other.siliconId && subsystemVendor==other.subsystemVendor &&
            subsystemDevice==other.subsystemDevice && speakerId==other.speakerId && channelIndex==other.channelIndex &&
            !strcmp(resourceId,other.resourceId);
    }
};
struct FirmwareBundle {
    FirmwareTarget target{};
    FirmwareFormat format{FirmwareFormat::None};
    const uint8_t* program{nullptr};size_t programBytes{0};
    const uint8_t* tuning{nullptr};size_t tuningBytes{0};
    bool matches(const FirmwareTarget& expected) const {
        if(!target.matches(expected) || bool(tuning)!=bool(tuningBytes))return false;
        if(format==FirmwareFormat::Deferred)return !program && !programBytes && !tuning && !tuningBytes;
        if(format==FirmwareFormat::None)return !program && !programBytes;
        return (format==FirmwareFormat::WMFW || format==FirmwareFormat::Vendor) && program && programBytes;
    }
    // matches() is selection only: the backend must validate format, device/DSP
    // revision, integrity and address ranges before writing any firmware bytes.
};
}
