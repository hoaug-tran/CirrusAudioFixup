// VoodooI2C publishes its backing ACPI device; endpoint and _DSD data stay owned
// here until driver discovery has copied the immutable board configuration.
#pragma once
#include "Platform/ACPI/ACPII2CResources.hpp"
#include "Platform/ACPI/ACPIPlatformProperties.hpp"
namespace cirrus::transport {
class I2CPlatformResources final {
    char mPath[256]{};
public:
    core::BusEndpoint endpoints[ACPII2CResources::kCapacity]{};
    size_t count{0};
    ACPIDeviceProperties properties;
    bool load(IOService* provider, const char*& reason) {
        reason = "I2C_ACPI_DEVICE_UNAVAILABLE";
        auto* acpi = provider ? support::acpiDeviceCast(provider->getProperty("acpi-device")) : nullptr;
        if (!acpi)
            return false;
        reason = "I2C_ACPI_RESOURCES_INVALID";
        OSObject* object = nullptr;
        const auto result = acpi->evaluateObject("_CRS", &object);
        auto* bytes = OSDynamicCast(OSData, object);
        ACPII2CResources resources;
        const bool valid = result == kIOReturnSuccess && bytes &&
            parseACPII2CResources(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(), resources);
        OSSafeReleaseNULL(object);
        if (!valid)
            return false;
        auto* address = OSDynamicCast(OSNumber, provider->getProperty("i2cAddress"));
        auto* width = OSDynamicCast(OSNumber, provider->getProperty("addrWidth"));
        bool matched = false;
        for (size_t i = 0; i < resources.count; ++i)
            matched |= address && width && address->unsigned64BitValue() == resources.endpoints[i].address &&
                       width->unsigned64BitValue() == (resources.endpoints[i].tenBit ? 10U : 7U);
        int length = sizeof(mPath);
        if (!matched || !provider->getPath(mPath, &length, gIOServicePlane)) {
            reason = "I2C_PROVIDER_ENDPOINT_UNMATCHED";
            return false;
        }
        if (properties.load(acpi) != kIOReturnSuccess && !properties.missingNativeMetadata()) {
            reason = properties.reason();
            return false;
        }
        count = resources.count;
        for (size_t i = 0; i < count; ++i)
            endpoints[i] = {core::BusTransportType::I2C, mPath, resources.endpoints[i].address, resources.endpoints[i].tenBit};
        reason = "I2C_ACPI_RESOURCES_RESOLVED";
        return true;
    }
};
}
