// Resolve firmware GPIO resources for input sampling and authorized reset pulses.
#pragma once
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/GPIO/AMDGPIOInput.hpp"
#include "Transport/GPIO/AMDGPIOOutput.hpp"
#include "Transport/SPI/Intel/IntelGPIOInput.hpp"
#include "Transport/SPI/Intel/IntelGPIOOutput.hpp"
#include <IOKit/IOLib.h>
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

inline bool isSupportedGPIOController(const char* hid) {
    if (!hid || !hid[0])
        return false;
    return amd::supportsGPIOInput(hid) ||
           !strcmp(hid, "INTC1055") || !strcmp(hid, "INT34C5") ||
           !strcmp(hid, "INTC1083") || !strcmp(hid, "INTC105E") ||
           !strcmp(hid, "INTC1056") || !strcmp(hid, "INTC1085");
}

inline bool extractGPIOProviderHID(IOACPIPlatformDevice* provider, char* hid, size_t maxLen) {
    if (!provider || !hid || maxLen < 2)
        return false;
    hid[0] = '\0';
    auto parseObject = [maxLen](OSObject* object, char* out) -> bool {
        if (!object || !out)
            return false;
        out[0] = '\0';
        if (auto* str = OSDynamicCast(OSString, object)) {
            snprintf(out, maxLen, "%s", str->getCStringNoCopy());
            return out[0] != '\0';
        }
        if (auto* data = OSDynamicCast(OSData, object)) {
            const size_t len = data->getLength() < maxLen - 1 ? data->getLength() : maxLen - 1;
            memcpy(out, data->getBytesNoCopy(), len);
            out[len] = '\0';
            return out[0] != '\0';
        }
        return false;
    };

    char candidate[16]{};
    OSObject* object = nullptr;
    if (provider->evaluateObject("_HID", &object) == kIOReturnSuccess && object) {
        if (parseObject(object, candidate)) {
            snprintf(hid, maxLen, "%s", candidate);
            if (isSupportedGPIOController(hid)) {
                OSSafeReleaseNULL(object);
                return true;
            }
        }
        OSSafeReleaseNULL(object);
    }
    if (provider->evaluateObject("_CID", &object) == kIOReturnSuccess && object) {
        if (parseObject(object, candidate)) {
            if (isSupportedGPIOController(candidate) || !hid[0])
                snprintf(hid, maxLen, "%s", candidate);
            if (isSupportedGPIOController(hid)) {
                OSSafeReleaseNULL(object);
                return true;
            }
        }
        OSSafeReleaseNULL(object);
    }
    if (auto* name = OSDynamicCast(OSData, provider->getProperty("name"))) {
        if (parseObject(name, candidate)) {
            if (isSupportedGPIOController(candidate) || !hid[0])
                snprintf(hid, maxLen, "%s", candidate);
            if (isSupportedGPIOController(hid))
                return true;
        }
    }
    if (auto* str = OSDynamicCast(OSString, provider->getProperty("name"))) {
        if (parseObject(str, candidate)) {
            if (isSupportedGPIOController(candidate) || !hid[0])
                snprintf(hid, maxLen, "%s", candidate);
            if (isSupportedGPIOController(hid))
                return true;
        }
    }
    return hid[0] != '\0';
}

inline bool gpioControllerPowered(IOACPIPlatformDevice* provider) {
    uint32_t power = 0;
    return (provider->getDeviceStatus() & 3) == 3 &&
        (provider->validateObject("_PSC") != kIOReturnSuccess ||
         (provider->evaluateInteger("_PSC", &power) == kIOReturnSuccess && !power));
}

struct IntelGPIOMemoryWindow final : intel::IntelGPIOWindow {
    IOMemoryMap* map;
    explicit IntelGPIOMemoryWindow(IOMemoryMap* m) : map(m) {}
    uint32_t read32(uint32_t offset) override {
        const auto val = *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset);
        OSSynchronizeIO();
        return val;
    }
    void write32(uint32_t offset, uint32_t value) override {
        *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset) = value;
        OSSynchronizeIO();
    }
};

inline bool readACPIGPIOInput(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin, bool activeLow,
                             IOService* owner, uint32_t& value) {
    const char* reason = "GPIO_RESOURCE_OR_CONTROLLER_UNAVAILABLE";
    ACPIGPIOResource gpio{};
    auto* provider = resolveACPIGPIOProvider(source, resource, pin, gpio);
    bool read = false;
    if (provider) {
        char hid[16]{};
        extractGPIOProviderHID(provider, hid, sizeof(hid));
        intel::IntelGPIOPad pad{};
        if (amd::supportsGPIOInput(hid)) {
            if (!gpioControllerPowered(provider)) {
                reason = "GPIO_CONTROLLER_NOT_POWERED";
            } else {
                auto* map = provider->mapDeviceMemoryWithIndex(0);
                reason = "GPIO_MEMORY_UNAVAILABLE";
                if (map && map->getVirtualAddress()) {
                    OSSynchronizeIO();
                    read = amd::readGPIOInput(reinterpret_cast<const volatile uint32_t*>(map->getVirtualAddress()),
                                              map->getLength(), gpio.pin, activeLow, value, reason);
                }
                OSSafeReleaseNULL(map);
            }
        } else if (intel::lookupIntelGPIOPad(hid, gpio.pin, pad)) {
            if (!gpioControllerPowered(provider)) {
                reason = "GPIO_CONTROLLER_NOT_POWERED";
            } else {
                auto* map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
                reason = "GPIO_MEMORY_UNAVAILABLE";
                if (map && map->getVirtualAddress() && !(map->getVirtualAddress() & 3)) {
                    IntelGPIOMemoryWindow window{map};
                    intel::IntelGPIOInputEvidence evidence{};
                    const auto result = intel::readIntelGPIOInput(window, map->getLength(), pad, activeLow, value, evidence);
                    reason = evidence.reason;
                    read = (result == kIOReturnSuccess);
                }
                OSSafeReleaseNULL(map);
            }
        } else {
            reason = "GPIO_CONTROLLER_UNSUPPORTED";
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
        char hid[16]{};
        extractGPIOProviderHID(provider, hid, sizeof(hid));
        intel::IntelGPIOPad pad{};
        if (amd::supportsGPIOInput(hid)) {
            if (!gpioControllerPowered(provider)) {
                reason = "GPIO_CONTROLLER_NOT_POWERED";
            } else {
                reason = "RESET_PROVIDER_BUSY";
                if (owner && provider->open(owner)) {
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
        } else if (intel::lookupIntelGPIOPad(hid, gpio.pin, pad)) {
            if (!gpioControllerPowered(provider)) {
                reason = "GPIO_CONTROLLER_NOT_POWERED";
            } else {
                reason = "RESET_PROVIDER_BUSY";
                if (owner && provider->open(owner)) {
                    auto* map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
                    reason = "RESET_MEMORY_UNAVAILABLE";
                    if (map && map->getVirtualAddress() && !(map->getVirtualAddress() & 3)) {
                        IntelGPIOMemoryWindow window{map};
                        intel::IntelGPIOOutput output;
                        const auto bindRes = output.bind(window, map->getLength(), pad, activeLow);
                        if (bindRes == kIOReturnSuccess) {
                            if (output.setActive(true) == kIOReturnSuccess) {
                                IOSleep(2);
                                output.setActive(false);
                                IOSleep(2);
                                complete = true;
                                reason = "RESET_PULSE_COMPLETE";
                            } else {
                                reason = output.evidence().reason;
                            }
                        } else {
                            reason = output.evidence().reason;
                        }
                    }
                    OSSafeReleaseNULL(map);
                    provider->close(owner);
                }
            }
        } else {
            reason = "GPIO_CONTROLLER_UNSUPPORTED";
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
