// ACPI serial-bus descriptors, per ACPICA amlresrc.h / rsserial.c.
// Keep I2C endpoint discovery independent of silicon and OEM tables.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace cirrus::transport {
struct ACPII2CResource {
    uint16_t address{0};
    bool tenBit{false};
    uint32_t clockHz{0};
    char source[128]{};
};
struct ACPII2CResources {
    static constexpr size_t kCapacity = 16;
    ACPII2CResource endpoints[kCapacity]{};
    size_t count{0};
};
inline bool parseACPII2CResources(const uint8_t* data, size_t length, ACPII2CResources& output) {
    if (!data || !length)
        return false;
    auto le16 = [](const uint8_t* p) { return uint16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8); };
    ACPII2CResources parsed{};
    for (size_t offset = 0; offset < length;) {
        const auto* d = data + offset;
        const bool large = (d[0] & 0x80) != 0;
        const size_t header = large ? 3 : 1;
        if (length - offset < header)
            return false;
        const size_t size = header + (large ? le16(d + 1) : d[0] & 7);
        if (size > length - offset)
            return false;
        if (!large && (d[0] >> 3) == 15) {
            if (size != 2 || offset + size != length || !parsed.count)
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
        if (!large && ((d[0] >> 3) == 6 || (d[0] >> 3) == 7))
            return false;
        if (d[0] == 0x8e) {
            // Common header + six I2C type bytes + a nonempty source string.
            if (size < 20 || (d[3] != 1 && d[3] != 2) || d[4] || d[5] != 1 || d[9] != 1 ||
                le16(d + 10) != 6 || (d[6] & ~uint8_t(6)) || !(d[6] & 2) ||
                (d[3] == 1 && (d[6] & 4)) || (le16(d + 7) & ~uint16_t(1)))
                return false;
            const size_t sourceLength = size - 18;
            if (parsed.count == ACPII2CResources::kCapacity || sourceLength > sizeof(parsed.endpoints[0].source) || d[size - 1])
                return false;
            auto& endpoint = parsed.endpoints[parsed.count];
            endpoint.clockHz = uint32_t(d[12]) | uint32_t(d[13]) << 8 | uint32_t(d[14]) << 16 | uint32_t(d[15]) << 24;
            endpoint.address = le16(d + 16);
            endpoint.tenBit = (le16(d + 7) & 1) != 0;
            if (!endpoint.clockHz || endpoint.address > (endpoint.tenBit ? 0x3ff : 0x7f))
                return false;
            for (size_t i = 0; i + 1 < sourceLength; ++i)
                if (d[18 + i] < 0x20 || d[18 + i] > 0x7e)
                    return false;
            memcpy(endpoint.source, d + 18, sourceLength);
            for (size_t i = 0; i < parsed.count; ++i)
                if (strcmp(endpoint.source, parsed.endpoints[i].source) ||
                    (endpoint.address == parsed.endpoints[i].address && endpoint.tenBit == parsed.endpoints[i].tenBit))
                    return false;
            ++parsed.count;
        }
        offset += size;
    }
    return false;
}
}
