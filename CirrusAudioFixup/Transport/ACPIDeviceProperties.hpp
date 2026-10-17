// Owned ACPI device-properties package and namespace reference resolution.
#pragma once
#include "Support/KernelDeviceCast.hpp"
#include "Transport/ACPIPath.hpp"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace cirrus::transport {
inline IOACPIPlatformDevice* resolveACPIDevice(IOACPIPlatformDevice* device, const char* source) {
    const auto* plane = IORegistryEntry::getPlane("IOACPIPlane");
    if (!device || !source || !plane)
        return nullptr;
    IORegistryEntry* scope = device;
    do {
        char path[256]{}, canonical[256]{}, wanted[256]{};
        int length = sizeof(path);
        if (!scope->getPath(path, &length, plane) || !canonicalACPIPath(path, canonical, sizeof(canonical)) ||
            !resolveACPIPath(canonical, source, wanted, sizeof(wanted)))
            return nullptr;
        auto* it = IORegistryIterator::iterateOver(IORegistryEntry::getRegistryRoot(), plane, kIORegistryIterateRecursively);
        if (!it)
            return nullptr;
        IOACPIPlatformDevice* found = nullptr;
        bool duplicate = false;
        while (auto* object = it->getNextObject()) {
            auto* candidate = support::acpiDeviceCast(object);
            length = sizeof(path);
            if (candidate && candidate->getPath(path, &length, plane) && canonicalACPIPath(path, canonical, sizeof(canonical)) &&
                !strcmp(canonical, wanted)) {
                if (found) {
                    duplicate = true;
                    break;
                }
                found = candidate;
            }
        }
        if (found && !duplicate)
            found->retain();
        it->release();
        if (duplicate)
            return nullptr;
        if (found)
            return found;
        if (*source == '\\' || *source == '^')
            break;
        scope = scope->getParentEntry(plane);
    } while (scope);
    return nullptr;
}
inline IOACPIPlatformDevice* resolveACPIReference(IOACPIPlatformDevice* scope, OSObject* object) {
    if (auto* device = support::acpiDeviceCast(object)) {
        device->retain();
        return device;
    }
    auto* path = OSDynamicCast(OSString, object);
    if (!path)
        return nullptr;
    const char* text = path->getCStringNoCopy();
    char canonical[256]{};
    if (!strncmp(text, "IOACPIPlane:", 12) || *text == '/')
        return canonicalACPIPath(text, canonical, sizeof(canonical)) ? resolveACPIDevice(scope, canonical) : nullptr;
    return resolveACPIDevice(scope, text);
}
class ACPIDeviceProperties {
public:
    ACPIDeviceProperties() = default;
    ACPIDeviceProperties(const ACPIDeviceProperties&) = delete;
    ACPIDeviceProperties& operator=(const ACPIDeviceProperties&) = delete;
    ~ACPIDeviceProperties() { OSSafeReleaseNULL(mObject); }
    IOReturn load(IOACPIPlatformDevice* device) {
        if (!device || mObject)
            return kIOReturnBadArgument;
        if (device->validateObject("_DSD") != kIOReturnSuccess)
            return kIOReturnSuccess;
        const auto result = device->evaluateObject("_DSD", &mObject);
        auto* root = OSDynamicCast(OSArray, mObject);
        if (result != kIOReturnSuccess || !root || root->getCount() > 64 || (root->getCount() & 1))
            return result == kIOReturnSuccess ? kIOReturnUnsupported : result;
        static constexpr uint8_t uuid[] = {0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01};
        for (unsigned i = 0; i < root->getCount(); i += 2) {
            auto* id = OSDynamicCast(OSData, root->getObject(i));
            if (!id || id->getLength() != sizeof(uuid))
                return kIOReturnUnsupported;
            if (memcmp(id->getBytesNoCopy(), uuid, sizeof(uuid)))
                continue;
            if (mProperties)
                return kIOReturnUnsupported;
            mProperties = OSDynamicCast(OSArray, root->getObject(i + 1));
            if (!mProperties || mProperties->getCount() > 128)
                return kIOReturnUnsupported;
            for (unsigned j = 0; j < mProperties->getCount(); ++j) {
                auto* pair = OSDynamicCast(OSArray, mProperties->getObject(j));
                auto* key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(0)) : nullptr;
                if (!key)
                    return kIOReturnUnsupported;
                for (unsigned k = 0; k < j; ++k) {
                    auto* previous = OSDynamicCast(OSArray, mProperties->getObject(k));
                    auto* previousKey = OSDynamicCast(OSString, previous->getObject(0));
                    if (key->isEqualTo(previousKey))
                        return kIOReturnUnsupported;
                }
            }
        }
        return kIOReturnSuccess;
    }
    OSObject* get(const char* name) const {
        for (unsigned i = 0; mProperties && i < mProperties->getCount(); ++i) {
            auto* pair = OSDynamicCast(OSArray, mProperties->getObject(i));
            auto* key = OSDynamicCast(OSString, pair->getObject(0));
            if (key->isEqualTo(name))
                return pair->getObject(1);
        }
        return nullptr;
    }

private:
    OSObject* mObject{nullptr};
    OSArray* mProperties{nullptr};
};
}
