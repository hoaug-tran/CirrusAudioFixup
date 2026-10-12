// Attach/controller/identity evidence is separate from chip runtime diagnostics.
// Preserve the first attach failure and rate-limit repeated identical failures.
#pragma once
#include <stdint.h>
#include <string.h>
namespace cirrus::diagnostics {
struct AttachmentFailure {
    const char* stage{"NONE"};
    const char* reason{"NONE"};
    int32_t result{0};
    uint32_t index{0};
    const char* indexKind{"ENDPOINT"};
};
struct AttachmentState {
    const char* communication{"NOT_PROBED"};
    const char* amplifier{"NOT_PROBED"};
    AttachmentFailure first{}, latest{};
    uint32_t failureCount{0};
    bool record(const char* stage, const char* reason, int32_t result, uint32_t index, const char* indexKind = "ENDPOINT") {
        if (!stage)
            stage = "UNKNOWN";
        if (!reason)
            reason = "UNSPECIFIED_FAILURE";
        if (!indexKind)
            indexKind = "UNKNOWN";
        const bool changed = strcmp(latest.stage, stage) || strcmp(latest.reason, reason) || latest.result != result ||
                             latest.index != index || strcmp(latest.indexKind, indexKind);
        latest = {stage, reason, result, index, indexKind};
        if (!failureCount)
            first = latest;
        if (failureCount != UINT32_MAX)
            ++failureCount;
        return changed || failureCount == 1 || !(failureCount % 32);
    }
};
}
