//
// Tuning.hpp
// Bounded parser for embedded Linux CS35L41 companion tuning parameters.
// The driver consumes these parameters; they are not DSP upload blocks.
// See LICENSE for distribution terms.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::devices::cs35l41::tuning {

struct Parameters {
    uint32_t pcmGain{17};
    bool overridden{false};
};

inline uint32_t readLittle32(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

// Layout follows Linux v6.12 cs35l41_hda.c: a 16-byte header, then records
// with index/type/size words. Record size includes its 12-byte header.
// Parse into a temporary result so a malformed late record cannot partially
// apply a gain. Missing data means the upstream default, not another channel.
inline bool parse(const uint8_t* data, size_t size, Parameters& output) {
    output = {};
    if (!data && !size)
        return true;
    if (!data || size < 16 || size > 65536 || (size & 3) ||
        readLittle32(data) != 0x109A4A35 || readLittle32(data + 4) != 1 ||
        readLittle32(data + 8) != size)
        return false;
    const uint32_t entries = readLittle32(data + 12);
    if (entries > (size - 16) / 12)
        return false;
    Parameters pending{};
    size_t offset = 16;
    for (uint32_t i = 0; i < entries; ++i) {
        if (size - offset < 12)
            return false;
        const uint32_t type = readLittle32(data + offset + 4);
        const uint32_t recordSize = readLittle32(data + offset + 8);
        if (recordSize < 12 || (recordSize & 3) || recordSize > size - offset)
            return false;
        if (type == 0) {
            if (recordSize != 16 || pending.overridden)
                return false;
            const uint32_t gain = readLittle32(data + offset + 12);
            if (gain > 20)
                return false;
            pending.pcmGain = gain;
            pending.overridden = true;
        }
        offset += recordSize;
    }
    // Unknown well-formed records are skipped. Unclaimed bytes are rejected:
    // a truncated entry count must not hide a gain or malformed record.
    if (offset != size)
        return false;
    output = pending;
    return true;
}

inline uint32_t encodeGain(uint32_t pcmGain) {
    return (pcmGain << 5) | 19U;
}

}
