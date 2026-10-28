
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
    const char* reason() const { return mReason; }
    uint32_t failureType() const { return mFailureType; }
    uint32_t nameStatus() const { return mNameStatus; }

private:
    bool contains(const void* pointer, size_t bytes) const {
        const auto address = uintptr_t(pointer);
        return pointer && address >= mData && address - mData <= mSize && bytes <= mSize - (address - mData);
    }
    OSObject* copy(const Object* object, unsigned depth) {
        if (depth >= 16)
            return fail("DEPTH_LIMIT");
        if (!mRemaining)
            return fail("OBJECT_LIMIT");
        if (!contains(object, sizeof(*object)) || uintptr_t(object) % alignof(Object))
            return fail("OBJECT_BOUNDS_INVALID");
        mFailureType = object->type;
        --mRemaining;
        switch (static_cast<ObjectType>(object->type)) {
        case ObjectType::Integer:
            return OSNumber::withNumber(object->integer.value, 64);
        case ObjectType::String: {
            const auto& string = object->string;
            if (!contains(string.pointer, size_t(string.length) + 1) || string.pointer[string.length] ||
                memchr(string.pointer, 0, string.length))
                return fail("STRING_INVALID");
            return OSString::withCString(string.pointer);
        }
        case ObjectType::Buffer: {
            const auto& buffer = object->buffer;
            if (buffer.length && !contains(buffer.pointer, buffer.length))
                return fail("BUFFER_BOUNDS_INVALID");
            return OSData::withBytes(buffer.pointer, buffer.length);
        }
        case ObjectType::Package: {
            const auto& package = object->package;
            if (package.count > mRemaining || (package.count && !contains(package.elements, size_t(package.count) * sizeof(Object))))
                return fail("PACKAGE_BOUNDS_INVALID");
            auto* array = OSArray::withCapacity(package.count);
            if (!array)
                return fail("ALLOCATION_FAILED");
            for (uint32_t i = 0; i < package.count; ++i) {
                auto* child = copy(package.elements + i, depth + 1);
                const bool hadChild = child != nullptr;
                const bool ok = child && array->setObject(child);
                OSSafeReleaseNULL(child);
                if (!ok) {
                    array->release();
                    return hadChild ? fail("ALLOCATION_FAILED") : nullptr;
                }
            }
            return array;
        }
        case ObjectType::Reference:
            return copyReference(object);
        default:
            return fail("OBJECT_TYPE_UNSUPPORTED");
        }
    }
    __attribute__((noinline)) OSObject* copyReference(const Object* object) {
        if (object->reference.actualType != uint32_t(ObjectType::Device) &&
            object->reference.actualType != uint32_t(ObjectType::Package))
            return fail("REFERENCE_TYPE_UNSUPPORTED");
        if (!object->reference.handle || !mGetName)
            return fail("REFERENCE_HANDLE_INVALID");
        char name[256]{}, canonical[256]{};
        Buffer buffer{sizeof(name), name};
        mNameStatus = mGetName(object->reference.handle, kFullPathname, &buffer);
        if (mNameStatus)
            return fail("REFERENCE_NAME_LOOKUP_FAILED");
        if (!buffer.length || buffer.length > sizeof(name) || buffer.pointer != name ||
            !memchr(name, 0, buffer.length) || *name != '\\' ||
            !canonicalACPIPath(name, canonical, sizeof(canonical)))
            return fail("REFERENCE_PATH_INVALID");
        return OSString::withCString(canonical);
    }
    OSObject* fail(const char* reason) { mReason = reason; return nullptr; }
    const char* mReason{"ALLOCATION_FAILED"};
    uint32_t mFailureType{0};
    uint32_t mNameStatus{0};
    uintptr_t mData;
    size_t mSize;
    GetName mGetName;
    size_t mRemaining{4096};
};
}
