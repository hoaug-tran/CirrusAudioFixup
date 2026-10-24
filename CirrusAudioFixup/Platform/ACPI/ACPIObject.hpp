// ACPICA external object ABI (actypes.h), independent of interpreter internals.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cirrus::transport::acpi {
enum class ObjectType : uint32_t { Integer = 1, String = 2, Buffer = 3, Package = 4, Device = 6, Reference = 20 };
inline constexpr uint32_t kFullPathname = 0;
union Object {
    uint32_t type;
    struct {
        uint32_t type;
        uint64_t value;
    } integer;
    struct {
        uint32_t type, length;
        const char* pointer;
    } string;
    struct {
        uint32_t type, length;
        const uint8_t* pointer;
    } buffer;
    struct {
        uint32_t type, count;
        const Object* elements;
    } package;
    struct {
        uint32_t type, actualType;
        void* handle;
    } reference;
    struct {
        uint32_t type, processorId;
        uint64_t pblkAddress;
        uint32_t pblkLength;
    } processor;
};
struct Buffer {
    size_t length;
    void* pointer;
};
static_assert(sizeof(void*) == 8 && sizeof(Object) == 24 && offsetof(Object, reference.handle) == 8,
              "Unsupported ACPICA external object ABI");
using GetHandle = uint32_t (*)(void*, const char*, void**);
using GetName = uint32_t (*)(void*, uint32_t, Buffer*);
using EvaluateObject = uint32_t (*)(void*, const char*, const void*, Buffer*);
}
