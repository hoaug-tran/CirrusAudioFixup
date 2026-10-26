// VoodooI2C publishes its backing ACPI device; endpoint and _DSD data stay owned
// here until driver discovery has copied the immutable board configuration.
#pragma once
#include "Platform/ACPI/ACPII2CResources.hpp"
#include "Platform/ACPI/ACPIPlatformProperties.hpp"
#include "Platform/ACPI/ACPIGPIOInput.hpp"
namespace cirrus::transport {
class I2CPlatformResources final : public platform::BoardDiscoveryContext::GPIOReader {
    IOACPIPlatformDevice* mACPI{nullptr};
    IOService* mOwner{nullptr};
    char mPath[256]{};
public:
    ~I2CPlatformResources() { OSSafeReleaseNULL(mACPI); }
    void setDiagnosticOwner(IOService* owner) { mOwner = owner; }
    bool readResource(uint32_t resource, uint32_t pin, bool low, uint32_t& value) override {
        return readACPIGPIOInput(mACPI, resource, pin, low, mOwner, value);
    }
    bool read(const char* property, size_t index, uint32_t& value) override {
        auto* tuples = OSDynamicCast(OSArray, properties.get(property));
        if (!tuples || tuples->getCount() % 4 || index >= tuples->getCount() / 4)
            return false;
        const unsigned i = unsigned(index * 4);
        auto* resource = OSDynamicCast(OSNumber, tuples->getObject(i + 1));
        auto* pin = OSDynamicCast(OSNumber, tuples->getObject(i + 2));
        auto* low = OSDynamicCast(OSNumber, tuples->getObject(i + 3));
        if (!resource || !pin || !low || resource->unsigned64BitValue() > UINT32_MAX ||
            pin->unsigned64BitValue() > UINT32_MAX || low->unsigned64BitValue() > 1)
            return false;
        auto* source = resolveACPIReference(mACPI, tuples->getObject(i));
        const bool result = readACPIGPIOInput(source, resource->unsigned32BitValue(), pin->unsigned32BitValue(),
                                              low->unsigned32BitValue() != 0, mOwner, value);
        OSSafeReleaseNULL(source);
        return result;
    }
    core::BusEndpoint endpoints[ACPII2CResources::kCapacity]{};
    size_t count{0};
    ACPIDeviceProperties properties;
    bool load(IOService* provider, const char*& reason) {
        count = 0;
        OSSafeReleaseNULL(mACPI);
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
        mACPI = acpi;
        mACPI->retain();
        count = resources.count;
        for (size_t i = 0; i < count; ++i)
            endpoints[i] = {core::BusTransportType::I2C, mPath, resources.endpoints[i].address, resources.endpoints[i].tenBit};
        reason = "I2C_ACPI_RESOURCES_RESOLVED";
        return true;
    }
};
}
