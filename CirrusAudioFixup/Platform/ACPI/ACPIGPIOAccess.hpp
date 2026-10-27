// Resolve firmware GPIO resources for input sampling and authorized reset pulses.
#pragma once
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/GPIO/AMDGPIOInput.hpp"
#include "Transport/GPIO/AMDGPIOOutput.hpp"
#include <IOKit/IOMemoryDescriptor.h>
namespace cirrus::transport {
inline IOACPIPlatformDevice* resolveACPIGPIOProvider(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin,
                                                    ACPIGPIOResource& gpio, bool reset = false) {
    OSObject* object = nullptr;
    const auto result = source ? source->evaluateObject("_CRS", &object) : kIOReturnNotFound;
    auto* data = OSDynamicCast(OSData, object);
    const bool valid = result == kIOReturnSuccess && data &&
        parseACPIGPIOResource(static_cast<const uint8_t*>(data->getBytesNoCopy()), data->getLength(), resource, pin, gpio, reset, reset);
    auto* provider = valid ? resolveACPIDevice(source, gpio.source) : nullptr;
    OSSafeReleaseNULL(object);
    return provider;
}
inline bool amdGPIOControllerReady(IOACPIPlatformDevice* provider, const char*& reason) {
    char hid[10]{};
    auto* name = OSDynamicCast(OSData, provider->getProperty("name"));
    const size_t length = name ? name->getLength() : 0;
    if (length && length < sizeof(hid))
        memcpy(hid, name->getBytesNoCopy(), length);
    reason = "GPIO_CONTROLLER_UNSUPPORTED";
    if (!amd::supportsGPIOInput(hid) || (length != strlen(hid) && length != strlen(hid) + 1))
        return false;
    uint32_t power = 0;
    reason = "GPIO_CONTROLLER_NOT_POWERED";
    return (provider->getDeviceStatus() & 3) == 3 &&
        (provider->validateObject("_PSC") != kIOReturnSuccess ||
         (provider->evaluateInteger("_PSC", &power) == kIOReturnSuccess && !power));
}
inline bool readACPIGPIOInput(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin, bool activeLow,
                             IOService* owner, uint32_t& value) {
    const char* reason = "GPIO_RESOURCE_OR_CONTROLLER_UNAVAILABLE";
    ACPIGPIOResource gpio{};
    auto* provider = resolveACPIGPIOProvider(source, resource, pin, gpio);
    bool read = false;
    if (provider) {
        if (amdGPIOControllerReady(provider, reason)) {
            auto* map = provider->mapDeviceMemoryWithIndex(0);
            reason = "GPIO_MEMORY_UNAVAILABLE";
            if (map && map->getVirtualAddress()) {
                OSSynchronizeIO();
                read = amd::readGPIOInput(reinterpret_cast<const volatile uint32_t*>(map->getVirtualAddress()),
                                          map->getLength(), gpio.pin, activeLow, value, reason);
            }
            OSSafeReleaseNULL(map);
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
// Reset endpoints are explicitly authorized by board metadata. The GPIO
// provider owns its resources; acquire an exclusive lease for the pulse.
inline bool pulseACPIGPIOReset(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin, bool activeLow,
                              IOService* owner) {
    const char* reason = "RESET_RESOURCE_INVALID";
    ACPIGPIOResource gpio{};
    auto* provider = resolveACPIGPIOProvider(source, resource, pin, gpio, true);
    bool complete = false;
    if (provider) {
        if (amdGPIOControllerReady(provider, reason)) {
            reason = "RESET_PROVIDER_BUSY";
            if (owner && !provider->isOpen() && provider->open(owner)) {
                auto* map = provider->mapDeviceMemoryWithIndex(0);
                reason = "RESET_MEMORY_UNAVAILABLE";
                if (map && map->getVirtualAddress() && !(map->getVirtualAddress() & 3) &&
                    amd::validGPIOOffset(map->getLength(), gpio.pin)) {
                    struct Window {
                        IOMemoryMap* map;
                        uint32_t read32(uint32_t offset) {
                            OSSynchronizeIO();
                            return *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset);
                        }
                        void write32(uint32_t offset, uint32_t value) {
                            *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset) = value;
                            OSSynchronizeIO();
                        }
                    } window{map};
                    complete = amd::pulseGPIOReset(window, map->getLength(), gpio.pin, activeLow,
                                                   [](uint32_t ms) { IOSleep(ms); }, reason);
                }
                OSSafeReleaseNULL(map);
                provider->close(owner);
            }
        }
        provider->release();
    }
    if (owner) {
        owner->setProperty("Cirrus_I2C_Reset_Reason", reason);
        owner->setProperty("Cirrus_I2C_Reset_Pin", uint64_t(gpio.pin), 32);
        owner->setProperty("Cirrus_I2C_Reset_Resource", uint64_t(resource), 32);
    }
    return complete;
}

}
