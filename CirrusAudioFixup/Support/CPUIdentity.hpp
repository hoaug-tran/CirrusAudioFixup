// Read-only x86 CPUID metadata. CPU vendor never selects a bus or AMP policy.
#pragma once
#include <stdint.h>
#include <string.h>
namespace cirrus::support {
inline const char* cpuVendorName(const char* vendor) {
    return vendor && !strcmp(vendor, "GenuineIntel") ? "INTEL" : vendor && !strcmp(vendor, "AuthenticAMD") ? "AMD" : "UNKNOWN";
}
struct CPUIdentity {
    char vendor[13]{};
    char brand[49]{};
};
inline CPUIdentity readCPUIdentity() {
    CPUIdentity info{};
#if defined(__x86_64__) || defined(__i386__)
    auto cpuid = [](uint32_t leaf, uint32_t* words) {
        __asm__ volatile("cpuid" : "=a"(words[0]), "=b"(words[1]), "=c"(words[2]), "=d"(words[3]) : "a"(leaf), "c"(0));
    };
    uint32_t words[4]{};
    cpuid(0, words);
    memcpy(info.vendor, words + 1, 4);
    memcpy(info.vendor + 4, words + 3, 4);
    memcpy(info.vendor + 8, words + 2, 4);
    cpuid(0x80000000, words);
    if (words[0] >= 0x80000004)
        for (uint32_t i = 0; i < 3; ++i) {
            cpuid(0x80000002 + i, words);
            memcpy(info.brand + i * 16, words, 16);
        }
#endif
    return info;
}
}
