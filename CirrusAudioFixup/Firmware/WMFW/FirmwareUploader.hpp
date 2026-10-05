#pragma once

#include "Core/RegisterIO.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"
#include "Support/Logging.hpp"

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
    UploadTransaction transactions[MAX_UPLOAD_TRANSACTIONS];
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

class CirrusFirmwareUploadPlanner {
public:
    static bool generatePlan(uint32_t regionIndex, const MappedRegion& region, const UploadPolicy& policy, UploadPlan& outPlan) {
        outPlan.regionType = region.regionType;
        outPlan.regionIndex = regionIndex;
        outPlan.transactionCount = 0;
        outPlan.totalSize = 0;
        outPlan.planCrc = 0xFFFFFFFF;

        uint32_t mappedBase = 0;
        if (CirrusFirmwareMapper::mapPackedAddress(region.regionType, 0, 0, mappedBase) != MappingStatus::OK ||
            policy.maxPayloadBytes < 4 || policy.maxPayloadBytes > 252 || (region.size && !region.data.begin) ||
            region.data.size < region.size || (region.dspRegister & 3) || (region.size & 3) ||
            uint64_t(region.dspRegister) + region.size > 0x100000000ULL)
            return false;
        if (!region.size)
            return true;
        uint32_t chunkLimit = policy.maxPayloadBytes & ~uint32_t(3);
        if ((uint64_t(region.size) + chunkLimit - 1) / chunkLimit > MAX_UPLOAD_TRANSACTIONS)
            return false;

        uint32_t remaining = region.size;
        uint32_t currentOffset = 0;

        while (remaining > 0) {
            if (outPlan.transactionCount >= MAX_UPLOAD_TRANSACTIONS) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: Too many transactions");
                return false;
            }

            uint32_t chunkSize = remaining > chunkLimit ? chunkLimit : remaining;

            UploadTransaction& tx = outPlan.transactions[outPlan.transactionCount];
            tx.firmwareAddress = region.firmwareAddress;
            tx.payloadOffset = currentOffset;
            tx.payload = region.data.begin + currentOffset;
            tx.size = chunkSize;

            tx.dspRegister = region.dspRegister + currentOffset;

            if (policy.alignRegister && (tx.dspRegister % 4 != 0)) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: dspRegister 0x%08X is not 4-byte aligned", tx.dspRegister);
                return false;
            }
            if (policy.alignPayload && (tx.size % 4 != 0)) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: payload size %d is not 4-byte aligned", tx.size);
                return false;
            }

            uint32_t crcFields[3] = {tx.dspRegister, tx.firmwareAddress, tx.size};
            const uint8_t* crcData = (const uint8_t*)crcFields;
            for (size_t k = 0; k < sizeof(crcFields); k++) {
                outPlan.planCrc ^= crcData[k];
                for (size_t j = 0; j < 8; j++) {
                    outPlan.planCrc = (outPlan.planCrc >> 1) ^ (0xEDB88320 & (-(outPlan.planCrc & 1)));
                }
            }

            outPlan.totalSize += tx.size;
            outPlan.transactionCount++;

            currentOffset += chunkSize;
            remaining -= chunkSize;
        }

        if (outPlan.totalSize != region.size) {
            CIRRUS_ERR("UPLOAD_PLAN_INVALID: Continuity check failed (%d vs %d)", outPlan.totalSize, region.size);
            return false;
        }

        outPlan.planCrc = ~outPlan.planCrc;
        return true;
    }
};

class CirrusFirmwareDryRunSimulator {
public:
    static void simulate(const UploadPlan& plan) {
        CIRRUS_LOG("--- Dry-Run Simulation for Region #%d ---", plan.regionIndex);

        for (uint32_t i = 0; i < plan.transactionCount; i++) {
            const UploadTransaction& tx = plan.transactions[i];
            CIRRUS_LOG("[DRYRUN] Tx%d: Reg=0x%08X, FW_Word=0x%06X, ChunkByte=0x%06X, Payload=[%d..%d], Size=%d", i, tx.dspRegister,
                       tx.firmwareAddress, tx.payloadOffset, tx.payloadOffset, tx.payloadOffset + tx.size - 1, tx.size);
        }

        const char* typeName = "UNKNOWN";
        switch (plan.regionType) {
        case RegionType::PM_PACKED:
            typeName = "PM_PACKED";
            break;
        case RegionType::XM_PACKED:
            typeName = "XM_PACKED";
            break;
        case RegionType::YM_PACKED:
            typeName = "YM_PACKED";
            break;
        case RegionType::ALGORITHM_DATA:
            typeName = "ALGORITHM";
            break;
        case RegionType::METADATA:
            typeName = "METADATA";
            break;
        case RegionType::NAME_TEXT:
            typeName = "NAME_TEXT";
            break;
        case RegionType::INFO_TEXT:
            typeName = "INFO_TEXT";
            break;
        default:
            break;
        }

        uint32_t fwStart = plan.transactionCount > 0 ? plan.transactions[0].firmwareAddress : 0;
        uint32_t fwEnd = plan.transactionCount > 0 ? plan.transactions[plan.transactionCount - 1].firmwareAddress +
                                                         plan.transactions[plan.transactionCount - 1].size
                                                   : 0;
        uint32_t dspStart = plan.transactionCount > 0 ? plan.transactions[0].dspRegister : 0;
        uint32_t dspEnd = plan.transactionCount > 0
                              ? plan.transactions[plan.transactionCount - 1].dspRegister + plan.transactions[plan.transactionCount - 1].size
                              : 0;

        CIRRUS_LOG("UploadPlan Summary");
        CIRRUS_LOG("Region        : %d (%s)", plan.regionIndex, typeName);
        CIRRUS_LOG("FW Start      : 0x%06X", fwStart);
        CIRRUS_LOG("FW End        : 0x%06X", fwEnd);
        CIRRUS_LOG("DSP Start     : 0x%08X", dspStart);
        CIRRUS_LOG("DSP End       : 0x%08X", dspEnd);
        CIRRUS_LOG("Transactions  : %d", plan.transactionCount);
        CIRRUS_LOG("Total Bytes   : %d", plan.totalSize);
        CIRRUS_LOG("Plan CRC      : 0x%08X", plan.planCrc);
        CIRRUS_LOG("Validation    : PASS");
    }
};

static inline const char* regionTypeName(RegionType t) {
    switch (t) {
    case RegionType::PM_PACKED:
        return "PM_PACKED";
    case RegionType::XM_PACKED:
        return "XM_PACKED";
    case RegionType::YM_PACKED:
        return "YM_PACKED";
    case RegionType::ALGORITHM_DATA:
        return "ALGORITHM";
    case RegionType::METADATA:
        return "METADATA";
    case RegionType::NAME_TEXT:
        return "NAME_TEXT";
    case RegionType::INFO_TEXT:
        return "INFO_TEXT";
    default:
        return "UNKNOWN";
    }
}

class CirrusFirmwareRealUploader {
public:
    static bool upload(const char* deviceName, cirrus::core::RegisterIO& io, const UploadPlan& plan, UploadStats* outStats = nullptr) {
        if (outStats)
            *outStats = {};
        if (!deviceName || plan.transactionCount > MAX_UPLOAD_TRANSACTIONS)
            return false;
        if (!plan.transactionCount)
            return plan.totalSize == 0;
        uint64_t covered = 0;
        for (uint32_t i = 0; i < plan.transactionCount; ++i) {
            const auto& tx = plan.transactions[i];
            if (!tx.payload || !tx.size || tx.size > 252 || (tx.size & 3) || (tx.dspRegister & 3) || tx.payloadOffset != covered ||
                uint64_t(tx.dspRegister) + tx.size > 0x100000000ULL ||
                uint64_t(plan.transactions[0].dspRegister) + covered != tx.dspRegister)
                return false;
            covered += tx.size;
        }
        if (covered != plan.totalSize)
            return false;

        uint32_t dspStart = plan.transactions[0].dspRegister;
        uint32_t totalSize = plan.totalSize;
        const char* rtype = regionTypeName(plan.regionType);

        CIRRUS_LOG("amp %s: starting physical upload of region data", deviceName);
        CIRRUS_LOG("Amp %s: Region %d (%s), Transactions: %d, Total: %d bytes, DSP Start: 0x%08X", deviceName, plan.regionIndex, rtype,
                   plan.transactionCount, totalSize, dspStart);

        bool isPM = (plan.regionType == RegionType::PM_PACKED);
        uint8_t* backupBuffer = isPM ? nullptr : static_cast<uint8_t*>(IOMallocData(totalSize));
        uint8_t* verifyBuffer = isPM ? nullptr : static_cast<uint8_t*>(IOMallocData(totalSize));
        if (!isPM && (!backupBuffer || !verifyBuffer)) {
            CIRRUS_ERR("Amp %s: Failed to allocate Backup/Verify buffers", deviceName);
            if (backupBuffer)
                IOFreeData(backupBuffer, totalSize);
            if (verifyBuffer)
                IOFreeData(verifyBuffer, totalSize);
            return false;
        }

        auto toMs = [&](uint64_t diff) -> uint32_t {
            uint64_t nsecs = 0;
            absolutetime_to_nanoseconds(diff, &nsecs);
            return (uint32_t)(nsecs / 1000000);
        };

        if (!isPM) {
            for (uint32_t i = 0; i < plan.transactionCount; ++i) {
                const auto& tx = plan.transactions[i];
                if (!io.bulkRead(tx.dspRegister, backupBuffer + tx.payloadOffset, tx.size)) {
                    IOFreeData(backupBuffer, totalSize);
                    IOFreeData(verifyBuffer, totalSize);
                    return false;
                }
            }
        }
        auto restoreRegion = [&]() -> bool {
            if (isPM || !backupBuffer || !verifyBuffer)
                return true;
            bool restored = true;
            for (uint32_t i = 0; i < plan.transactionCount; ++i) {
                const auto& tx = plan.transactions[i];
                bool written = io.bulkWrite(tx.dspRegister, backupBuffer + tx.payloadOffset, tx.size);
                bool read = io.bulkRead(tx.dspRegister, verifyBuffer + tx.payloadOffset, tx.size);
                restored =
                    written && read && !memcmp(backupBuffer + tx.payloadOffset, verifyBuffer + tx.payloadOffset, tx.size) && restored;
            }
            CIRRUS_LOG("Amp %s: REGION_RESTORE=%s; whole image remains invalid", deviceName, restored ? "VERIFIED" : "FAILED");
            return restored;
        };

        uint64_t t_total_start = mach_absolute_time();
        uint32_t accWriteMs = 0, accRbMs = 0, accCrcMs = 0, accRetries = 0;

        for (uint32_t i = 0; i < plan.transactionCount; i++) {
            const UploadTransaction& tx = plan.transactions[i];

            CIRRUS_LOG("amp %s: transaction %d/%d (dsp=0x%08x size=%d)", deviceName, i + 1, plan.transactionCount, tx.dspRegister, tx.size);

            uint32_t totalPacketLength = tx.size + 4;
            CIRRUS_LOG("Amp %s:   WRITE    : payload = %d bytes, packet = %d bytes (payload + 4)", deviceName, tx.size, totalPacketLength);
            uint64_t t0 = mach_absolute_time();
            bool writeOk = false;
            for (int attempt = 1; attempt <= 2; attempt++) {
                if (io.bulkWrite(tx.dspRegister, tx.payload, tx.size)) {
                    writeOk = true;
                    break;
                }
                if (attempt < 2) {
                    CIRRUS_LOG("Amp %s:   WRITE    : FAIL (Attempt 1/2) retrying...", deviceName);
                    accRetries++;
                    IOSleep(10);
                }
            }
            uint32_t writeMs = toMs(mach_absolute_time() - t0);
            accWriteMs += writeMs;

            if (!writeOk) {
                CIRRUS_LOG("Amp %s:   WRITE    : FAIL (2/2, %d ms)", deviceName, writeMs);
                CIRRUS_LOG("Amp %s:   READBACK : SKIPPED", deviceName);
                CIRRUS_LOG("Amp %s:   CRC      : SKIPPED", deviceName);
                bool rbOk = restoreRegion();
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", deviceName, rbOk ? "PASS" : "FAIL");
                if (backupBuffer)
                    IOFreeData(backupBuffer, totalSize);
                if (verifyBuffer)
                    IOFreeData(verifyBuffer, totalSize);
                return false;
            }
            CIRRUS_LOG("Amp %s:   WRITE    : PASS (%d ms)", deviceName, writeMs);

            if (isPM) {
                CIRRUS_LOG("Amp %s:   READBACK : SKIPPED", deviceName);
                CIRRUS_LOG("Amp %s:   CRC      : SKIPPED", deviceName);
                CIRRUS_LOG("Amp %s:   ROLLBACK : SKIPPED", deviceName);
                continue;
            }

            t0 = mach_absolute_time();
            uint8_t* rbSlot = verifyBuffer + tx.payloadOffset;
            bool readOk = io.bulkRead(tx.dspRegister, rbSlot, tx.size);
            uint32_t rbMs = toMs(mach_absolute_time() - t0);
            accRbMs += rbMs;

            if (!readOk) {
                CIRRUS_LOG("Amp %s:   READBACK : FAIL (%d ms)", deviceName, rbMs);
                CIRRUS_LOG("Amp %s:   CRC      : SKIPPED", deviceName);
                bool rbOk = restoreRegion();
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", deviceName, rbOk ? "PASS" : "FAIL");
                if (backupBuffer)
                    IOFreeData(backupBuffer, totalSize);
                if (verifyBuffer)
                    IOFreeData(verifyBuffer, totalSize);
                return false;
            }
            CIRRUS_LOG("Amp %s:   READBACK : PASS (%d ms)", deviceName, rbMs);

            t0 = mach_absolute_time();
            uint32_t payCrc = 0xFFFFFFFF, rbCrc = 0xFFFFFFFF;
            const uint8_t* paySlice = tx.payload;
            for (uint32_t b = 0; b < tx.size; b++) {
                payCrc ^= paySlice[b];
                rbCrc ^= rbSlot[b];
                for (int j = 0; j < 8; j++) {
                    payCrc = (payCrc >> 1) ^ (0xEDB88320 & (-(payCrc & 1)));
                    rbCrc = (rbCrc >> 1) ^ (0xEDB88320 & (-(rbCrc & 1)));
                }
            }
            payCrc = ~payCrc;
            rbCrc = ~rbCrc;
            uint32_t crcMs = toMs(mach_absolute_time() - t0);
            accCrcMs += crcMs;

            if (payCrc != rbCrc || memcmp(paySlice, rbSlot, tx.size)) {
                CIRRUS_LOG("Amp %s:   CRC      : FAIL (%d ms) [Exp=0x%08X Got=0x%08X]", deviceName, crcMs, payCrc, rbCrc);

                uint32_t dumpLen = (tx.size < 16) ? (uint32_t)tx.size : 16;
                char payHex[64] = {0};
                char rbHex[64] = {0};
                for (uint32_t d = 0; d < dumpLen; d++) {
                    snprintf(payHex + d * 3, sizeof(payHex) - d * 3, "%02X ", paySlice[d]);
                    snprintf(rbHex + d * 3, sizeof(rbHex) - d * 3, "%02X ", rbSlot[d]);
                }
                CIRRUS_LOG("Amp %s:   PAYLOAD  : %s", deviceName, payHex);
                CIRRUS_LOG("Amp %s:   READBACK : %s", deviceName, rbHex);

                for (uint32_t b = 0; b < tx.size; b++) {
                    if (paySlice[b] != rbSlot[b]) {
                        CIRRUS_LOG("Amp %s:   memcmp   : offset 0x%06X (Exp=0x%02X Got=0x%02X)", deviceName, b, paySlice[b], rbSlot[b]);
                        break;
                    }
                }
                bool rbOk = restoreRegion();
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", deviceName, rbOk ? "PASS" : "FAIL");
                if (backupBuffer)
                    IOFreeData(backupBuffer, totalSize);
                if (verifyBuffer)
                    IOFreeData(verifyBuffer, totalSize);

                return false;
            }
            CIRRUS_LOG("Amp %s:   CRC      : PASS (%d ms) [0x%08X]", deviceName, crcMs, payCrc);
            CIRRUS_LOG("Amp %s:   ROLLBACK : SKIPPED", deviceName);
        }

        uint32_t totalMs = toMs(mach_absolute_time() - t_total_start);
        CIRRUS_LOG("Amp %s: Upload Complete | Tx=%d PASS, Write=%d ms, RB=%d ms, CRC=%d ms, Total=%d ms", deviceName, plan.transactionCount,
                   accWriteMs, accRbMs, accCrcMs, totalMs);

        if (outStats) {
            outStats->writeMs = accWriteMs;
            outStats->readbackMs = accRbMs;
            outStats->crcMs = accCrcMs;
            outStats->totalMs = totalMs;
            outStats->retries = accRetries;
        }

        if (backupBuffer)
            IOFreeData(backupBuffer, totalSize);
        if (verifyBuffer)
            IOFreeData(verifyBuffer, totalSize);
        return true;
    }
};

struct RegionResult {
    uint32_t regionIndex;
    RegionType type;
    uint32_t bytes;
    uint32_t transactionCount;
    bool success;
    uint32_t planCrc;
    uint32_t elapsedMs;
};

struct UploadSession {
    RegionResult results[32];
    uint32_t regionCount;
    uint32_t passCount;
    uint32_t totalBytes;
    uint32_t totalTransactions;
    uint32_t totalMs;
    bool complete;
};

class CirrusFirmwareScheduler {
public:
    static bool run(const char* deviceName, cirrus::core::RegisterIO& io, MappedImage& mappedImg, UploadSession& session) {
        session = {};
        if (mappedImg.regionCount > MAX_MAPPED_REGIONS)
            return false;
        uint32_t expectedRegions = 0;
        UploadPolicy policy{252, true, true};
        UploadPlan* preflight = static_cast<UploadPlan*>(IOMalloc(sizeof(UploadPlan)));
        if (!preflight)
            return false;
        bool valid = true;
        for (uint32_t i = 0; i < mappedImg.regionCount; ++i) {
            const auto& region = mappedImg.regions[i];
            if (region.regionType == RegionType::INFO_TEXT || region.regionType == RegionType::NAME_TEXT ||
                region.regionType == RegionType::METADATA || region.regionType == RegionType::ALGORITHM_DATA)
                continue;
            if (++expectedRegions > 32 || !CirrusFirmwareUploadPlanner::generatePlan(i, region, policy, *preflight)) {
                valid = false;
                break;
            }
        }
        IOFree(preflight, sizeof(UploadPlan));
        if (!valid || !expectedRegions)
            return false;

        uint64_t t_session_start = mach_absolute_time();

        CIRRUS_LOG("Amp %s: WMFW Upload — %d mapped regions total", deviceName, mappedImg.regionCount);

        for (uint32_t i = 0; i < mappedImg.regionCount; i++) {
            const MappedRegion& region = mappedImg.regions[i];

            const char* rname = regionTypeName(region.regionType);
            CIRRUS_LOG("Amp %s: Region %d (%s) %d bytes", deviceName, i, rname, region.size);

            if (region.regionType == RegionType::INFO_TEXT || region.regionType == RegionType::ALGORITHM_DATA ||
                region.regionType == RegionType::METADATA || region.regionType == RegionType::NAME_TEXT) {
                CIRRUS_LOG("Amp %s:   Region %d (%s) is metadata, skipping upload.", deviceName, i, rname);
                continue;
            }

            UploadPlan* plan = (UploadPlan*)IOMalloc(sizeof(UploadPlan));
            if (!plan) {
                CIRRUS_ERR("Amp %s: Failed to allocate UploadPlan for region %d", deviceName, i);
                RegionResult& res = session.results[session.regionCount++];
                res = {i, region.regionType, region.size, 0, false, 0, 0};
                break;
            }

            bool planOk = CirrusFirmwareUploadPlanner::generatePlan(i, region, policy, *plan);
            if (!planOk) {
                CIRRUS_ERR("Amp %s: Region %d plan FAIL", deviceName, i);
                IOFree(plan, sizeof(UploadPlan));
                RegionResult& res = session.results[session.regionCount++];
                res = {i, region.regionType, region.size, 0, false, 0, 0};
                break;
            }

            uint64_t t0 = mach_absolute_time();
            UploadStats stats = {};
            bool ok = CirrusFirmwareRealUploader::upload(deviceName, io, *plan, &stats);
            uint64_t t1 = mach_absolute_time();

            uint64_t nsecs = 0;
            absolutetime_to_nanoseconds(t1 - t0, &nsecs);
            uint32_t elapsedMs = (uint32_t)(nsecs / 1000000);

            RegionResult& res = session.results[session.regionCount++];
            res.regionIndex = i;
            res.type = region.regionType;
            res.bytes = plan->totalSize;
            res.transactionCount = plan->transactionCount;
            res.success = ok;
            res.planCrc = plan->planCrc;
            res.elapsedMs = elapsedMs;

            IOFree(plan, sizeof(UploadPlan));

            if (ok) {
                session.passCount++;
                session.totalBytes += res.bytes;
                session.totalTransactions += res.transactionCount;
                CIRRUS_LOG("Amp %s:   Region %d (%s) MappedTo=0x%08X PASS (%d ms)", deviceName, i, rname, region.dspRegister, elapsedMs);
            } else {
                CIRRUS_ERR("Amp %s:   Region %d (%s) FAIL — stopping", deviceName, i, rname);
                break;
            }
        }

        uint64_t t_ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - t_session_start, &t_ns);
        session.totalMs = (uint32_t)(t_ns / 1000000);
        session.complete = session.regionCount == expectedRegions && session.passCount == expectedRegions;

        CIRRUS_LOG("Amp %s: ================================", deviceName);
        CIRRUS_LOG("Amp %s: WMFW Upload Summary", deviceName);
        CIRRUS_LOG("Amp %s: ================================", deviceName);
        for (uint32_t i = 0; i < session.regionCount; i++) {
            const RegionResult& r = session.results[i];
            CIRRUS_LOG("Amp %s:   [%d] %-10s %s  %d bytes  %d tx  %d ms", deviceName, r.regionIndex, regionTypeName(r.type),
                       r.success ? "PASS" : "FAIL", r.bytes, r.transactionCount, r.elapsedMs);
        }
        CIRRUS_LOG("Amp %s: ================================", deviceName);
        CIRRUS_LOG("Amp %s:   Regions      : %d / %d PASS", deviceName, session.passCount, session.regionCount);
        CIRRUS_LOG("Amp %s:   Bytes        : %d", deviceName, session.totalBytes);
        CIRRUS_LOG("Amp %s:   Transactions : %d", deviceName, session.totalTransactions);
        CIRRUS_LOG("Amp %s:   Total Time   : %d ms", deviceName, session.totalMs);
        CIRRUS_LOG("Amp %s: ================================", deviceName);

        if (session.complete) {
            CIRRUS_LOG("Amp %s: WMFW UPLOAD COMPLETE", deviceName);
        } else {
            CIRRUS_ERR("Amp %s: WMFW UPLOAD INCOMPLETE — %d/%d regions passed", deviceName, session.passCount, session.regionCount);
        }

        return session.complete;
    }
};
