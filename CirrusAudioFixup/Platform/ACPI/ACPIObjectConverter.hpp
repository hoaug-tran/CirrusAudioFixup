// Copy a bounded interpreter result into owned kernel objects.
#pragma once
#include "Platform/ACPI/ACPIObject.hpp"
#include "Platform/ACPI/ACPIPath.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace cirrus::transport::acpi {
class ObjectConverter {
public:
    ObjectConverter(const void* data, size_t size, GetName getName) : mData(uintptr_t(data)), mSize(size), mGetName(getName) {}
    OSObject* copy(const Object* object) { return copy(object, 0); }

private:
    bool contains(const void* pointer, size_t bytes) const {
        const auto address = uintptr_t(pointer);
        return pointer && address >= mData && address - mData <= mSize && bytes <= mSize - (address - mData);
    }
    OSObject* copy(const Object* object, unsigned depth) {
        if (depth >= 16 || !mRemaining || !contains(object, sizeof(*object)) || uintptr_t(object) % alignof(Object))
            return nullptr;
        --mRemaining;
        switch (static_cast<ObjectType>(object->type)) {
        case ObjectType::Integer:
            return OSNumber::withNumber(object->integer.value, 64);
        case ObjectType::String: {
            const auto& string = object->string;
            if (!contains(string.pointer, size_t(string.length) + 1) || string.pointer[string.length] ||
                memchr(string.pointer, 0, string.length))
                return nullptr;
            return OSString::withCString(string.pointer);
        }
        case ObjectType::Buffer: {
            const auto& buffer = object->buffer;
            if (buffer.length && !contains(buffer.pointer, buffer.length))
                return nullptr;
            return OSData::withBytes(buffer.pointer, buffer.length);
        }
        case ObjectType::Package: {
            const auto& package = object->package;
            if (package.count > mRemaining || (package.count && !contains(package.elements, size_t(package.count) * sizeof(Object))))
                return nullptr;
            auto* array = OSArray::withCapacity(package.count);
            if (!array)
                return nullptr;
            for (uint32_t i = 0; i < package.count; ++i) {
                auto* child = copy(package.elements + i, depth + 1);
                const bool ok = child && array->setObject(child);
                OSSafeReleaseNULL(child);
                if (!ok) {
                    array->release();
                    return nullptr;
                }
            }
            return array;
        }
        case ObjectType::Reference:
            return copyReference(object);
        default:
            return nullptr;
        }
    }
    // Keep namespace buffers out of every recursive package frame.
    __attribute__((noinline)) OSObject* copyReference(const Object* object) {
        // _DSD may also reference package data nodes under another UUID.
        // Both remain namespace paths; GPIO consumers resolve devices only.
        if ((object->reference.actualType != uint32_t(ObjectType::Device) &&
             object->reference.actualType != uint32_t(ObjectType::Package)) ||
            !object->reference.handle || !mGetName)
            return nullptr;
        char name[256]{}, canonical[256]{};
        Buffer buffer{sizeof(name), name};
        if (mGetName(object->reference.handle, kFullPathname, &buffer) || !buffer.length || buffer.length > sizeof(name) ||
            buffer.pointer != name || !memchr(name, 0, buffer.length) || *name != '\\' ||
            !canonicalACPIPath(name, canonical, sizeof(canonical)))
            return nullptr;
        return OSString::withCString(canonical);
    }
    uintptr_t mData;
    size_t mSize;
    GetName mGetName;
    size_t mRemaining{4096};
};
}
