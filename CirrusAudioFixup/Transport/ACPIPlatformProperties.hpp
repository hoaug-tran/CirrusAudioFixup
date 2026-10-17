// Convert evaluated ACPI property packages into the shared bounded data view.
#pragma once
#include "Platform/BoardDiscovery.hpp"
#include "Transport/ACPIDeviceProperties.hpp"
namespace cirrus::transport {
class ACPIPlatformProperties final : public platform::PlatformProperties {
    const ACPIDeviceProperties& mProperties;
public:
    explicit ACPIPlatformProperties(const ACPIDeviceProperties& properties) : mProperties(properties) {}
    bool array(const char* name, uint32_t* output, size_t count) const override {
        auto* values = OSDynamicCast(OSArray, mProperties.get(name));
        if (!output || !values || values->getCount() != count || count > 16)
            return false;
        for (size_t i = 0; i < count; ++i) {
            auto* value = OSDynamicCast(OSNumber, values->getObject(unsigned(i)));
            if (!value || value->unsigned64BitValue() > UINT32_MAX)
                return false;
            output[i] = value->unsigned32BitValue();
        }
        return true;
    }
};
}
