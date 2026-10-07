#pragma once

#include "Firmware/WMFW/WMFWFormat.hpp"

#include <stddef.h>
#include <stdint.h>

namespace cirrus::firmware::wmfw {

constexpr size_t kMaxUploadTransactions = 1024;
#define MAX_UPLOAD_TRANSACTIONS 1024

struct UploadPolicy {
    uint32_t maxPayloadBytes;
    bool alignRegister;
    bool alignPayload;
};

struct UploadTransaction {
    uint32_t dspRegister;
    uint32_t firmwareAddress;
    uint32_t payloadOffset;
    uint32_t size;
    const uint8_t* payload;
};

struct UploadPlan {
    RegionType regionType;
    uint32_t regionIndex;
    UploadTransaction transactions[kMaxUploadTransactions];
    uint32_t transactionCount;
    uint32_t totalSize;
    uint32_t planCrc;
};

struct UploadStats {
    uint32_t writeMs;
    uint32_t readbackMs;
    uint32_t crcMs;
    uint32_t totalMs;
    uint32_t retries;
};

}

using UploadPolicy = cirrus::firmware::wmfw::UploadPolicy;
using UploadTransaction = cirrus::firmware::wmfw::UploadTransaction;
using UploadPlan = cirrus::firmware::wmfw::UploadPlan;
using UploadStats = cirrus::firmware::wmfw::UploadStats;
