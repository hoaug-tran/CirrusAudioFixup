// Cirrus EFI calibration ABI, shared by smart amplifiers. No machine data.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cirrus::firmware {
struct EFICalibration {
    int32_t ambient{0};
    uint32_t resistance{0}, status{0};
    static uint32_t little32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static bool select(const uint8_t* data, size_t bytes, uint64_t uid, size_t index, EFICalibration& out) {
        out = {};
        if (!data || bytes < 8)
            return false;
        const uint32_t count = little32(data + 4);
        if (!count || count > 128 || count > (bytes - 8) / 20)
            return false;
        const uint8_t* selected = nullptr;
        auto target = [](const uint8_t* p) { return uint64_t(little32(p)) | uint64_t(little32(p + 4)) << 32; };
        auto occupied = [](const uint8_t* p) { return little32(p + 8) || little32(p + 12); };
        if (uid) {
            for (uint32_t i = 0; i < count; ++i) {
                const auto* entry = data + 8 + size_t(i) * 20;
                if (occupied(entry) && target(entry) == uid) {
                    if (selected)
                        return false;
                    selected = entry;
                }
            }
        }
        if (!selected && index < count) {
            const auto* entry = data + 8 + index * 20;
            if (occupied(entry) && (!uid || !target(entry)))
                selected = entry;
        }
        if (!selected)
            return false;
        const uint32_t resistance = uint32_t(selected[18]) | uint32_t(selected[19]) << 8;
        if (selected[17] != 1 || !resistance || resistance == 0xFFFF)
            return false;
        out = {int8_t(selected[16]), resistance, selected[17]};
        return true;
    }
};
}
