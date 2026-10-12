// Compare AML namespace paths with IOACPIPlane paths without guessing names.
#pragma once
#include <stddef.h>
#include <string.h>
namespace cirrus::transport {
inline bool canonicalACPIPath(const char* path, char* out, size_t capacity) {
    if (!path || !out || capacity < 2)
        return false;
    const char* p = path;
    if (!strncmp(p, "IOACPIPlane:", 12))
        p += 12;
    if (*p != '/' && *p != '\\')
        return false;
    ++p;
    if (*p == '\\')
        ++p; // Some IOACPIPlane roots retain the AML root prefix.
    size_t n = 0;
    out[n++] = '\\';
    while (*p) {
        if (*p == '/' || *p == '.' || *p == '@' || (*p >= '0' && *p <= '9'))
            return false;
        char segment[5]{};
        size_t bytes = 0;
        while (*p && *p != '/' && *p != '.' && *p != '@') {
            if (bytes == 4 || !((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
                return false;
            segment[bytes++] = *p++;
        }
        while (*p && *p != '/' && *p != '.')
            ++p; // Registry location is not AML name.
        while (bytes > 1 && segment[bytes - 1] == '_')
            --bytes;
        if (n > 1) {
            if (n + 1 >= capacity)
                return false;
            out[n++] = '.';
        }
        if (n + bytes >= capacity)
            return false;
        memcpy(out + n, segment, bytes);
        n += bytes;
        if (*p) {
            ++p;
            if (!*p)
                return false;
        }
    }
    out[n] = 0;
    return true;
}
// Candidate namespace scope is explicit; owner performs ACPI ancestor lookup.
inline bool resolveACPIPath(const char* scope, const char* source, char* out, size_t capacity) {
    if (!scope || !source || !*source)
        return false;
    // ResourceSource is an AML NameString, not an IORegistry path/location.
    for (const char* c = source; *c; ++c)
        if (*c == '/' || *c == '@')
            return false;
    if (*source == '\\')
        return canonicalACPIPath(source, out, capacity);
    char joined[256]{};
    size_t length = strlen(scope);
    if (length >= sizeof(joined))
        return false;
    memcpy(joined, scope, length + 1);
    while (*source == '^') {
        if (length <= 1)
            return false;
        while (length > 1 && joined[length - 1] != '.')
            --length;
        if (length > 1)
            --length;
        joined[length] = 0;
        ++source;
    }
    if (!*source || length + strlen(source) + 2 > sizeof(joined))
        return false;
    if (length > 1)
        joined[length++] = '.';
    memcpy(joined + length, source, strlen(source) + 1);
    return canonicalACPIPath(joined, out, capacity);
}
}
