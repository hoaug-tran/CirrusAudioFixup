// Decode a raw ACPI _CRS ResourceTemplate without executing AML or accessing
// hardware. Output owns its source paths; failed parses leave output unchanged.
// Layout reference: ACPICA amlresrc.h / rsserial.c (ACPI serial bus descriptor).
#pragma once
#include "Transport/SPIController.hpp"

namespace cirrus::transport {

struct ACPISPIResource {
    SPIEndpoint endpoint{};
    uint8_t sourceIndex{0};
    bool shared{false};
    char source[128]{};
};

struct ACPISPIResources {
    static constexpr size_t kMaxEndpoints = 16;
    ACPISPIResource endpoints[kMaxEndpoints]{};
    size_t count{0};
};

enum class ACPISPIResult { Success, NoSPI, Malformed, Unsupported, TooManyEndpoints };

inline ACPISPIResult parseACPISPIResources(const uint8_t* data, size_t length, ACPISPIResources& output, bool* hasI2C = nullptr) {
    if (!data || !length)
        return ACPISPIResult::Malformed;
    ACPISPIResources parsed{};
    bool foundI2C = false;
    auto le16 = [](const uint8_t* p) -> uint16_t { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); };
    size_t offset = 0;
    while (offset < length) {
        const uint8_t tag = data[offset];
        const bool large = (tag & 0x80) != 0;
        const size_t header = large ? 3 : 1;
        if (length - offset < header)
            return ACPISPIResult::Malformed;
        const size_t payload = large ? le16(data + offset + 1) : (tag & 7);
        if (payload > length - offset - header)
            return ACPISPIResult::Malformed;
        const size_t total = header + payload;
        const uint8_t* d = data + offset;
        if (!large && (tag >> 3) == 15) {
            if (payload != 1 || offset + total != length)
                return ACPISPIResult::Malformed;
            // Zero checksum means not supplied. Otherwise verify the full template.
            if (d[1]) {
                uint8_t sum = 0;
                for (size_t i = 0; i < length; ++i)
                    sum = uint8_t(sum + data[i]);
                if (sum)
                    return ACPISPIResult::Malformed;
            }
            output = parsed;
            if (hasI2C)
                *hasI2C = foundI2C;
            return parsed.count ? ACPISPIResult::Success : ACPISPIResult::NoSPI;
        }
        if (tag == 0x8e) {
            if (payload < 9)
                return ACPISPIResult::Malformed;
            if (d[5] == 1) {
                const size_t typeLength = le16(d + 10);
                if (typeLength < 6 || typeLength > total - 12 || total - 12 - typeLength < 2)
                    return ACPISPIResult::Malformed;
                const uint8_t* source = d + 12 + typeLength;
                const size_t sourceLength = total - 12 - typeLength;
                if (sourceLength < 2 || source[sourceLength - 1] != 0)
                    return ACPISPIResult::Malformed;
                foundI2C = true;
            }
            if (d[5] == 2) {
                // Common header is 12 bytes; SPI type data starts at byte 12.
                const size_t typeLength = le16(d + 10);
                if (typeLength < 9 || typeLength > total - 12 || total - 12 - typeLength < 2)
                    return ACPISPIResult::Malformed;
                if ((d[3] != 1 && d[3] != 2) || d[9] != 1)
                    return ACPISPIResult::Unsupported;
                // Master controller, consumer, 4-wire, active-low, 8-bit only.
                if ((d[6] & ~uint8_t(6)) || !(d[6] & 2) || (d[3] == 1 && (d[6] & 4)) || le16(d + 7) || d[16] != 8)
                    return ACPISPIResult::Unsupported;
                if (d[17] > 1 || d[18] > 1)
                    return ACPISPIResult::Malformed;
                const uint32_t speed = uint32_t(d[12]) | (uint32_t(d[13]) << 8) | (uint32_t(d[14]) << 16) | (uint32_t(d[15]) << 24);
                if (!speed)
                    return ACPISPIResult::Malformed;
                const size_t sourceOffset = 12 + typeLength;
                const size_t sourceLength = total - sourceOffset;
                if (sourceLength > sizeof(parsed.endpoints[0].source))
                    return ACPISPIResult::Unsupported;
                if (d[total - 1] != 0)
                    return ACPISPIResult::Malformed;
                for (size_t i = sourceOffset; i + 1 < total; ++i) {
                    if (!d[i] || d[i] < 0x20 || d[i] > 0x7e)
                        return ACPISPIResult::Malformed;
                }
                if (parsed.count == ACPISPIResources::kMaxEndpoints)
                    return ACPISPIResult::TooManyEndpoints;
                auto& e = parsed.endpoints[parsed.count++];
                e.endpoint = {le16(d + 19), speed, uint8_t((d[18] << 1) | d[17])};
                e.sourceIndex = d[4];
                e.shared = (d[6] & 4) != 0;
                for (size_t i = 0; i < sourceLength; ++i)
                    e.source[i] = char(d[sourceOffset + i]);
                // Vendor bytes cannot silently change the framing contract.
                if (typeLength != 9)
                    return ACPISPIResult::Unsupported;
            }
        }
        // Dependent alternatives require a resource-selection policy, not merging.
        if (!large && ((tag >> 3) == 6 || (tag >> 3) == 7))
            return ACPISPIResult::Unsupported;
        offset += total;
    }
    return ACPISPIResult::Malformed; // Missing EndTag.
}

}
