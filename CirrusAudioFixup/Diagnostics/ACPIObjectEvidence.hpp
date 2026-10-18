// Bounded, data-only package evidence. Never retain firmware device references.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <string.h>
namespace cirrus::diagnostics {
class ACPIObjectEvidence {
public:
    static void publish(IOService* owner, const char* prefix, OSObject* object) {
        unsigned count = 0;
        bool partial = false;
        node(owner, prefix, object, "0", 0, count, partial);
        char key[128]{};
        snprintf(key, sizeof(key), "%s_Nodes", prefix);
        owner->setProperty(key, uint64_t(count), 32);
        snprintf(key, sizeof(key), "%s_Status", prefix);
        owner->setProperty(key, partial ? "PARTIAL_LIMIT" : "COMPLETE");
    }

private:
    static void node(IOService* owner, const char* prefix, OSObject* object, const char* path, unsigned depth, unsigned& count,
                     bool& partial) {
        if (count >= 256 || depth > 6) {
            partial = true;
            return;
        }
        char key[128]{}, value[384]{};
        const unsigned index = count++;
        if (auto* array = OSDynamicCast(OSArray, object)) {
            snprintf(value, sizeof(value), "%s OSArray count=%u", path, array->getCount());
        } else if (auto* data = OSDynamicCast(OSData, object)) {
            char hex[129]{};
            const auto* bytes = static_cast<const uint8_t*>(data->getBytesNoCopy());
            const size_t length = data->getLength() < 64 ? data->getLength() : 64;
            for (size_t i = 0; i < length; ++i)
                snprintf(hex + i * 2, 3, "%02x", bytes[i]);
            snprintf(value, sizeof(value), "%s OSData bytes=%llu hex=%s%s", path, static_cast<unsigned long long>(data->getLength()), hex,
                     data->getLength() > length ? " TRUNCATED" : "");
            partial |= data->getLength() > length;
        } else if (auto* string = OSDynamicCast(OSString, object)) {
            const char* text = string->getCStringNoCopy();
            snprintf(value, sizeof(value), "%s OSString value=%.128s", path, text);
            partial |= strlen(text) > 128;
        } else if (auto* number = OSDynamicCast(OSNumber, object)) {
            snprintf(value, sizeof(value), "%s OSNumber value=0x%016llx", path,
                     static_cast<unsigned long long>(number->unsigned64BitValue()));
        } else {
            snprintf(value, sizeof(value), "%s %s", path, object ? "REFERENCE_OR_OTHER" : "NULL");
        }
        snprintf(key, sizeof(key), "%s_Node_%03u", prefix, index);
        owner->setProperty(key, value);
        if (auto* array = OSDynamicCast(OSArray, object)) {
            for (unsigned i = 0; i < array->getCount(); ++i) {
                if (count >= 256 || depth >= 6) {
                    partial = true;
                    break;
                }
                char child[80]{};
                snprintf(child, sizeof(child), "%s.%u", path, i);
                node(owner, prefix, array->getObject(i), child, depth + 1, count, partial);
            }
        }
    }
};
}
