// Attachment-owned board metadata. Never replaces AML or publishes properties
// on another device. A registered module supplies verified facts; consumers
// merge only absent firmware fields and retain strict schema checks.
#pragma once
#include "Platform/ACPI/ACPIDeviceProperties.hpp"

namespace cirrus::support {
class ACPIBoardMetadata {
    OSArray* mNodes{OSArray::withCapacity(4)};
public:
    ~ACPIBoardMetadata() { OSSafeReleaseNULL(mNodes); }
    static bool path(IOACPIPlatformDevice* device, char (&output)[256]) {
        char raw[256]{};
        int length = sizeof(raw);
        return device && device->getPath(raw, &length, IORegistryEntry::getPlane("IOACPIPlane")) &&
               transport::canonicalACPIPath(raw, output, sizeof(output));
    }
    OSArray* node(IOACPIPlatformDevice* device) {
        char name[256]{};
        if (!mNodes || !path(device, name))
            return nullptr;
        auto* entries = OSArray::withCapacity(16);
        auto* pair = OSArray::withCapacity(2);
        auto* key = OSString::withCString(name);
        const bool ok = entries && pair && key && pair->setObject(key) && pair->setObject(entries) && mNodes->setObject(pair);
        OSSafeReleaseNULL(key);
        OSSafeReleaseNULL(pair);
        auto* result = ok ? entries : nullptr;
        OSSafeReleaseNULL(entries);
        return result;
    }
    static bool property(OSArray* entries, const char* name, OSObject* value) {
        auto* key = OSString::withCString(name);
        auto* pair = OSArray::withCapacity(2);
        const bool ok = entries && key && pair && value && pair->setObject(key) && pair->setObject(value) && entries->setObject(pair);
        OSSafeReleaseNULL(key);
        OSSafeReleaseNULL(pair);
        return ok;
    }
    static bool appendNumber(OSArray* values, uint32_t value) {
        auto* number = OSNumber::withNumber(value, 32);
        const bool ok = number && values && values->setObject(number);
        OSSafeReleaseNULL(number);
        return ok;
    }
    static bool array(OSArray* entries, const char* name, const uint32_t* values, size_t count) {
        if (!values || !count || count > 16)
            return false;
        auto* array = OSArray::withCapacity(unsigned(count));
        bool ok = array != nullptr;
        for (size_t i = 0; ok && i < count; ++i)
            ok = appendNumber(array, values[i]);
        ok = ok && property(entries, name, array);
        OSSafeReleaseNULL(array);
        return ok;
    }
    // One CRS resource may represent a shared line used by several endpoints.
    // Native SPI selections are represented by leading zero entries.
    static bool references(OSArray* entries, const char* name, IOACPIPlatformDevice* source,
                           uint16_t resource, uint16_t pin, bool activeLow, unsigned count, unsigned nativePrefix = 0) {
        char namePath[256]{};
        if (!count || count > 16 || nativePrefix > 16 || !path(source, namePath))
            return false;
        auto* values = OSArray::withCapacity(count * 4 + nativePrefix);
        bool ok = values != nullptr;
        for (unsigned i = 0; ok && i < nativePrefix; ++i)
            ok = appendNumber(values, 0);
        for (unsigned i = 0; ok && i < count; ++i) {
            auto* reference = OSString::withCString(namePath);
            ok = reference && values->setObject(reference) && appendNumber(values, resource) &&
                 appendNumber(values, pin) && appendNumber(values, activeLow);
            OSSafeReleaseNULL(reference);
        }
        ok = ok && property(entries, name, values);
        OSSafeReleaseNULL(values);
        return ok;
    }
    bool publish(IOService* owner) const { return mNodes && owner && owner->setProperty("Cirrus_Board_Metadata", mNodes); }
};
}
