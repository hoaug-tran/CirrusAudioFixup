// Resolve firmware GPIO references and sample an existing controller input.
#pragma once
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/GPIO/AMDGPIOInput.hpp"
#include <IOKit/IOMemoryDescriptor.h>
namespace cirrus::transport {
inline bool readACPIGPIOInput(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin, bool activeLow,
                             IOService* owner, uint32_t& value) {
    const char* reason = "GPIO_RESOURCE_INVALID";
    OSObject* object = nullptr;
    const auto result = source ? source->evaluateObject("_CRS", &object) : kIOReturnNotFound;
    auto* data = OSDynamicCast(OSData, object);
    ACPIGPIOResource gpio{};
    const bool valid = result == kIOReturnSuccess && data &&
        parseACPIGPIOResource(static_cast<const uint8_t*>(data->getBytesNoCopy()), data->getLength(), resource, pin, gpio, false);
    auto* provider = valid ? resolveACPIDevice(source, gpio.source) : nullptr;
    OSSafeReleaseNULL(object);
    bool read = false;
    if (valid && !provider)
        reason = "GPIO_CONTROLLER_NOT_FOUND";
    if (provider) {
        char hid[10]{};
        auto* name = OSDynamicCast(OSData, provider->getProperty("name"));
        const size_t length = name ? name->getLength() : 0;
        if (length && length < sizeof(hid))
            memcpy(hid, name->getBytesNoCopy(), length);
        reason = "GPIO_CONTROLLER_UNSUPPORTED";
        if (amd::supportsGPIOInput(hid) && (length == strlen(hid) || length == strlen(hid) + 1)) {
            uint32_t power = 0;
            reason = "GPIO_CONTROLLER_NOT_POWERED";
            if ((provider->getDeviceStatus() & 3) == 3 &&
                (provider->validateObject("_PSC") != kIOReturnSuccess ||
                 (provider->evaluateInteger("_PSC", &power) == kIOReturnSuccess && !power))) {
                auto* map = provider->mapDeviceMemoryWithIndex(0);
                reason = "GPIO_MEMORY_UNAVAILABLE";
                if (map && map->getVirtualAddress()) {
                    OSSynchronizeIO();
                    read = amd::readGPIOInput(reinterpret_cast<const volatile uint32_t*>(map->getVirtualAddress()),
                                              map->getLength(), gpio.pin, activeLow, value, reason);
                }
                OSSafeReleaseNULL(map);
            }
        }
        provider->release();
    }
    if (owner) {
        owner->setProperty("Cirrus_GPIO_Input_Reason", reason);
        owner->setProperty("Cirrus_GPIO_Input_Resource", uint64_t(resource), 32);
        owner->setProperty("Cirrus_GPIO_Input_Pin", uint64_t(gpio.pin), 32);
        if (read)
            owner->setProperty("Cirrus_GPIO_Input_Value", uint64_t(value), 32);
        else
            owner->removeProperty("Cirrus_GPIO_Input_Value");
    }
    return read;
}
}
