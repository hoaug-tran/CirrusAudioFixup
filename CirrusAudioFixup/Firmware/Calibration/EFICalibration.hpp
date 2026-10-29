// Cirrus EFI calibration ABI, shared by smart amplifiers. No machine data.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cirrus::firmware {
struct EFICalibration {
    // Linux cs-amp-lib EFI ABI: 8-byte header followed by 20-byte records.
    static constexpr size_t kHeaderBytes = 8, kRecordBytes = 20;
    int32_t ambient{0};
    uint32_t resistance{0}, status{0};
    static uint32_t little32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static bool recordCount(const uint8_t* data, size_t bytes, uint32_t& count) {
        count = 0;
        if (!data || bytes < kHeaderBytes)
            return false;
        const uint32_t declared = little32(data + 4);
        if (declared > (bytes - kHeaderBytes) / kRecordBytes)
            return false;
        count = declared;
        return true;
    }
    static uint64_t target(const uint8_t* record) { return uint64_t(little32(record)) | uint64_t(little32(record + 4)) << 32; }
    static bool occupied(const uint8_t* record) { return little32(record + 8) || little32(record + 12); }
    static EFICalibration decode(const uint8_t* record) {
        return {int8_t(record[16]), uint32_t(record[18]) | uint32_t(record[19]) << 8, record[17]};
    }
    struct RecordSelection {
        const uint8_t* record{nullptr};
        uint32_t index{UINT32_MAX};
        bool duplicateTarget{false};
    };
    // Decode/selection is shared; callers retain their silicon UID and validity policies.
    static RecordSelection selectRecord(const uint8_t* data, size_t bytes, uint64_t uid, size_t index) {
        uint32_t count = 0;
        RecordSelection selected;
        if (!recordCount(data, bytes, count))
            return selected;
        if (uid)
            for (uint32_t i = 0; i < count; ++i) {
                const auto* entry = data + kHeaderBytes + size_t(i) * kRecordBytes;
                if (occupied(entry) && target(entry) == uid) {
                    if (selected.record)
                        selected.duplicateTarget = true;
                    else {
                        selected.record = entry;
                        selected.index = i;
                    }
                }
            }
        if (!selected.record && index < count) {
            const auto* entry = data + kHeaderBytes + index * kRecordBytes;
            if (occupied(entry) && (!uid || !target(entry))) {
                selected.record = entry;
                selected.index = uint32_t(index);
            }
        }
        return selected;
    }
    static bool select(const uint8_t* data, size_t bytes, uint64_t uid, size_t index, EFICalibration& out) {
        out = {};
        uint32_t count = 0;
        if (!recordCount(data, bytes, count) || !count || count > 128)
            return false;
        const auto selected = selectRecord(data, bytes, uid, index);
        if (!selected.record || selected.duplicateTarget)
            return false;
        const auto value = decode(selected.record);
        if (value.status != 1 || !value.resistance || value.resistance == 0xFFFF)
            return false;
        out = value;
        return true;
    }
};
}
