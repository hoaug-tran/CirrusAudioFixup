// Resolve firmware GPIO resources for input sampling and authorized reset pulses.
#pragma once
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/GPIO/AMDGPIOInput.hpp"
#include "Transport/GPIO/AMDGPIOOutput.hpp"
#include "Transport/SPI/Intel/IntelGPIOInput.hpp"
#include "Transport/SPI/Intel/IntelGPIOOutput.hpp"
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
inline bool intelGPIOControllerReady(IOACPIPlatformDevice* provider, char* hidOut, size_t maxLen, const char*& reason) {
    char hid[16]{};
    auto* name = OSDynamicCast(OSData, provider->getProperty("name"));
    const size_t length = name ? name->getLength() : 0;
    if (length && length < sizeof(hid))
        memcpy(hid, name->getBytesNoCopy(), length);
    if (!length || !hid[0]) {
        OSObject* object = nullptr;
        if (provider->evaluateObject("_HID", &object) == kIOReturnSuccess) {
            auto* s = OSDynamicCast(OSString, object);
            const char* str = s ? s->getCStringNoCopy() : nullptr;
            if (str && strlen(str) < sizeof(hid))
                strncpy(hid, str, sizeof(hid) - 1);
        }
        OSSafeReleaseNULL(object);
    }
    const bool supported = !strcmp(hid, "INTC1055") || !strcmp(hid, "INT34C5") ||
                           !strcmp(hid, "INTC1056") || !strcmp(hid, "INTC1085") ||
                           !strcmp(hid, "INTC1083") || !strcmp(hid, "INTC105E");
    if (!supported) {
        reason = "GPIO_CONTROLLER_UNSUPPORTED";
        return false;
    }
    if (hidOut && maxLen) {
        strncpy(hidOut, hid, maxLen - 1);
        hidOut[maxLen - 1] = '\0';
    }
    uint32_t power = 0;
    reason = "GPIO_CONTROLLER_NOT_POWERED";
    return (provider->getDeviceStatus() & 3) == 3 &&
        (provider->validateObject("_PSC") != kIOReturnSuccess ||
         (provider->evaluateInteger("_PSC", &power) == kIOReturnSuccess && !power));
}
struct IntelI2CGPIOWindow final : intel::IntelGPIOWindow {
    IOMemoryMap* map{nullptr};
    explicit IntelI2CGPIOWindow(IOMemoryMap* m) : map(m) {}
    uint32_t read32(uint32_t offset) override {
        OSSynchronizeIO();
        return *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset);
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
        char intelHid[16]{};
        if (amdGPIOControllerReady(provider, reason)) {
            auto* map = provider->mapDeviceMemoryWithIndex(0);
            reason = "GPIO_MEMORY_UNAVAILABLE";
            if (map && map->getVirtualAddress()) {
                OSSynchronizeIO();
                read = amd::readGPIOInput(reinterpret_cast<const volatile uint32_t*>(map->getVirtualAddress()),
                                          map->getLength(), gpio.pin, activeLow, value, reason);
            }
            OSSafeReleaseNULL(map);
        } else if (intelGPIOControllerReady(provider, intelHid, sizeof(intelHid), reason)) {
            intel::IntelGPIOPad pad{};
            if (intel::lookupIntelGPIOPad(intelHid, gpio.pin, pad)) {
                auto* map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
                reason = "GPIO_MEMORY_UNAVAILABLE";
                if (map && map->getVirtualAddress()) {
                    IntelI2CGPIOWindow window{map};
                    intel::IntelGPIOInputEvidence evidence{};
                    read = intel::readIntelGPIOInput(window, map->getLength(), pad, activeLow, value, evidence) == kIOReturnSuccess;
                    reason = evidence.reason;
                }
                OSSafeReleaseNULL(map);
            } else {
                reason = "GPIO_PIN_UNSUPPORTED";
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
// Reset endpoints are explicitly authorized by board metadata. The GPIO
// provider owns its resources; acquire an exclusive lease for the pulse.
inline bool pulseACPIGPIOReset(IOACPIPlatformDevice* source, uint32_t resource, uint32_t pin, bool activeLow,
                              IOService* owner) {
    const char* reason = "RESET_RESOURCE_INVALID";
    ACPIGPIOResource gpio{};
    auto* provider = resolveACPIGPIOProvider(source, resource, pin, gpio, true);
    bool complete = false;
    if (provider) {
        char intelHid[16]{};
        if (amdGPIOControllerReady(provider, reason)) {
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
        } else if (intelGPIOControllerReady(provider, intelHid, sizeof(intelHid), reason)) {
            intel::IntelGPIOPad pad{};
            if (intel::lookupIntelGPIOPad(intelHid, gpio.pin, pad)) {
                reason = "RESET_PROVIDER_BUSY";
                const bool opened = !owner || provider->open(owner);
                if (opened) {
                    auto* map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
                    reason = "RESET_MEMORY_UNAVAILABLE";
                    if (map && map->getVirtualAddress() && !(map->getVirtualAddress() & 3)) {
                        IntelI2CGPIOWindow window{map};
                        intel::IntelGPIOOutput output;
                        if (output.bind(window, map->getLength(), pad, activeLow) == kIOReturnSuccess) {
                            output.setActive(true);
                            IOSleep(2);
                            output.setActive(false);
                            IOSleep(5);
                            complete = true;
                            reason = "RESET_PULSE_COMPLETE";
                        } else if (!strcmp(output.evidence().reason, "PAD_NOT_GPIO_OUTPUT") &&
                                   !(output.evidence().ownership & (0xfU << ((pad.bit % 8) * 4))) &&
                                   !(output.evidence().lockTx & (1U << pad.bit))) {
                            const uint32_t padbar = window.read32(0x0c);
                            const uint32_t offset = padbar + uint32_t(pad.pad) * 16;
                            uint32_t cfg = window.read32(offset);
                            cfg &= ~(1U << 8); // clear GPIOTXDIS to enable output
                            cfg &= ~(0xfU << 10); // set PMODE = 0 (GPIO mode)
                            window.write32(offset, (cfg & ~1U) | (activeLow ? 0 : 1U));
                            IOSleep(2);
                            window.write32(offset, (cfg & ~1U) | (activeLow ? 1U : 0));
                            IOSleep(5);
                            complete = true;
                            reason = "RESET_PULSE_CONFIGURED";
                        } else {
                            reason = output.evidence().reason;
                        }
                    }
                    OSSafeReleaseNULL(map);
                    if (owner && opened)
                        provider->close(owner);
                }
            } else {
                reason = "RESET_PIN_UNSUPPORTED";
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
