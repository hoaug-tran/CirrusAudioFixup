#pragma once

#include "Firmware/WMFW/WMFWFormat.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::firmware::wmfw {

// Maximum number of bus transactions batched into a single region upload plan
constexpr size_t kMaxUploadTransactions = 1024;
#define MAX_UPLOAD_TRANSACTIONS 1024

// Bus transfer constraints (payload chunk size, register and payload alignment)
struct UploadPolicy {
    uint32_t maxPayloadBytes;
    bool alignRegister;
    bool alignPayload;
};

// Single hardware write transaction dispatched over physical transport
struct UploadTransaction {
    uint32_t dspRegister;
    uint32_t firmwareAddress;
    uint32_t payloadOffset;
    uint32_t size;
    const uint8_t* payload;
};

// Staged sequence of bus transactions required to flash a firmware memory region
struct UploadPlan {
    RegionType regionType;
    uint32_t regionIndex;
    UploadTransaction transactions[kMaxUploadTransactions];
    uint32_t transactionCount;
    uint32_t totalSize;
    uint32_t planCrc;
};

// Telemetry recording bus timing and retry counters during firmware upload
struct UploadStats {
    uint32_t writeMs;
    uint32_t readbackMs;
    uint32_t crcMs;
    uint32_t totalMs;
    uint32_t retries;
};

} // namespace cirrus::firmware::wmfw

// Global aliases for legacy compatibility
using UploadPolicy = cirrus::firmware::wmfw::UploadPolicy;
using UploadTransaction = cirrus::firmware::wmfw::UploadTransaction;
using UploadPlan = cirrus::firmware::wmfw::UploadPlan;
using UploadStats = cirrus::firmware::wmfw::UploadStats;
