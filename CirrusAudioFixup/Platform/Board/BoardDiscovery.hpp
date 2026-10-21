// Optional driver-owned interpretation of firmware properties. The host owns
// the result until every borrower has stopped; no silicon/vendor dispatch here.
#pragma once
#include "Platform/Board/BoardConfiguration.hpp"
namespace cirrus::platform {
class PlatformProperties {
public:
    virtual ~PlatformProperties() = default;
    // Distinguish an omitted optional field from a present malformed value.
    virtual bool contains(const char* name) const = 0;
    // Require exactly count integer elements, each representable as uint32_t.
    virtual bool array(const char* name, uint32_t* output, size_t count) const = 0;
};
struct BoardDiscoveryContext {
    BoardIdentity identity;
    const core::BusEndpoint* endpoints;
    size_t count;
    const PlatformProperties& properties;
    class GPIOReader {
    public:
        virtual ~GPIOReader() = default;
        virtual bool read(const char* property, size_t index, uint32_t& value) = 0;
    };
    GPIOReader* gpio{nullptr};
};
class DiscoveredBoard {
public:
    virtual ~DiscoveredBoard() = default;
    BoardConfiguration configuration{};
};
}
