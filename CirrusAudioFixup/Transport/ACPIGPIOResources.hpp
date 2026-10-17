// Decode an evaluated GPIO _CRS connection by GPIO-resource and pin index.
// Offsets follow ACPICA AML_RESOURCE_GPIO, not an unevaluated AML template.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cirrus::transport {
struct ACPIGPIOResource {
    uint16_t pin{0};
    uint8_t sourceIndex{0};
    char source[128]{};
};
inline bool parseACPIGPIOResource(const uint8_t* data, size_t length, uint32_t resourceIndex, uint32_t pinIndex, ACPIGPIOResource& output) {
    if (!data || !length)
        return false;
    auto le16 = [](const uint8_t* p) -> uint16_t { return uint16_t(p[0]) | uint16_t(p[1]) << 8; };
    ACPIGPIOResource parsed{};
    bool found = false;
    uint32_t index = 0;
    for (size_t offset = 0; offset < length;) {
        const auto* d = data + offset;
        const bool large = (*d & 0x80) != 0;
        const size_t header = large ? 3 : 1;
        if (length - offset < header)
            return false;
        const size_t size = header + (large ? le16(d + 1) : (*d & 7));
        if (size > length - offset)
            return false;
        if (!large && (*d >> 3) == 15) {
            if (size != 2 || offset + size != length || !found)
                return false;
            if (d[1]) {
                uint8_t sum = 0;
                for (size_t i = 0; i < length; ++i)
                    sum = uint8_t(sum + data[i]);
                if (sum)
                    return false;
            }
            output = parsed;
            return true;
        }
        if (!large && ((*d >> 3) == 6 || (*d >> 3) == 7))
            return false;
        if (*d == 0x8c && index++ == resourceIndex) {
            // Only exclusive GpioIo output connections are writable here.
            if (size < 23 || d[3] != 1 || d[4] != 1 || le16(d + 5) != 1 || le16(d + 7) != 2 || d[16] != 0)
                return false;
            const size_t pins = le16(d + 14), source = le16(d + 17);
            const size_t vendor = le16(d + 19), vendorSize = le16(d + 21);
            if (pins < 23 || source <= pins || source > size || ((source - pins) & 1) || pinIndex >= (source - pins) / 2 || vendorSize ||
                (vendor && vendor != size))
                return false;
            const size_t sourceSize = size - source;
            if (sourceSize < 2 || sourceSize > sizeof(parsed.source) || d[size - 1])
                return false;
            for (size_t i = 0; i + 1 < sourceSize; ++i) {
                if (d[source + i] < 0x20 || d[source + i] > 0x7e)
                    return false;
                parsed.source[i] = char(d[source + i]);
            }
            parsed.pin = le16(d + pins + pinIndex * 2);
            parsed.sourceIndex = d[16];
            found = true;
        }
        offset += size;
    }
    return false;
}
}
