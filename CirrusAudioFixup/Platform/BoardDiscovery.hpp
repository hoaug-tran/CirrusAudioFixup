// Optional driver-owned interpretation of firmware properties. The host owns
// the result until every borrower has stopped; no silicon/vendor dispatch here.
#pragma once
#include "Platform/BoardConfiguration.hpp"
namespace cirrus::platform {
class PlatformProperties {
public:
    virtual ~PlatformProperties() = default;
    // Require exactly count integer elements, each representable as uint32_t.
    virtual bool array(const char* name, uint32_t* output, size_t count) const = 0;
};
struct BoardDiscoveryContext {
    BoardIdentity identity;
    const core::BusEndpoint* endpoints;
    size_t count;
    const PlatformProperties& properties;
};
class DiscoveredBoard {
public:
    virtual ~DiscoveredBoard() = default;
    BoardConfiguration configuration{};
};
}
