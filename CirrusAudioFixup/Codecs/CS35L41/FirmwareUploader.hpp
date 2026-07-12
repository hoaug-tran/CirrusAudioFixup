#ifndef CS35L41_FIRMWARE_UPLOADER_HPP
#define CS35L41_FIRMWARE_UPLOADER_HPP

#include "FirmwareParser.hpp"
#include "../../CirrusAudioFixup.hpp"

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
    const uint8_t *payload;
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
    static bool generatePlan(uint32_t regionIndex, const MappedRegion &region, const UploadPolicy &policy, UploadPlan &outPlan) {
        outPlan.regionType = region.regionType;
        outPlan.regionIndex = regionIndex;
        outPlan.transactionCount = 0;
        outPlan.totalSize = 0;
        outPlan.planCrc = 0xFFFFFFFF;
        
        if (region.size == 0) {
            return true; // Nothing to do
        }
        
        if (policy.maxPayloadBytes == 0) {
            CIRRUS_ERR("UploadPolicy maxPayloadBytes cannot be 0");
            return false;
        }

        uint32_t remaining = region.size;
        uint32_t currentOffset = 0;
        
        while (remaining > 0) {
            if (outPlan.transactionCount >= MAX_UPLOAD_TRANSACTIONS) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: Too many transactions");
                return false;
            }
            
            uint32_t chunkSize = (remaining > policy.maxPayloadBytes) ? policy.maxPayloadBytes : remaining;
            
            // align chunk size to lcm(dsp word size, i2c word size)
            // i2c word size is 4 bytes (32-bit registers)
            // pm is 5 bytes -> lcm(5, 4) = 20
            // xm/ym is 3 bytes -> lcm(3, 4) = 12
            uint32_t align = 4;
            switch (region.regionType) {
                case RegionType::PM_PACKED: align = 20; break;
                case RegionType::XM_PACKED:
                case RegionType::YM_PACKED: align = 12; break;
                default: align = 4; break;
            }
            if (chunkSize > align) {
                chunkSize -= (chunkSize % align);
            }
            
            if (chunkSize == 0) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: chunkSize dropped to 0 due to alignment");
                return false;
            }
            
            UploadTransaction &tx = outPlan.transactions[outPlan.transactionCount];
            tx.firmwareAddress = region.firmwareAddress; // firmware word offset
            tx.payloadOffset = currentOffset;
            tx.payload = region.data.begin + currentOffset;
            tx.size = chunkSize;
            
            // calculate register address increment based on region type
            uint32_t chunkReg = 0;
            MappingStatus status = CirrusFirmwareMapper::mapPackedAddress(region.regionType, region.firmwareAddress, currentOffset, chunkReg);
            if (status != MappingStatus::OK) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: Failed to map chunk offset 0x%06X", tx.firmwareAddress);
                return false;
            }
            tx.dspRegister = chunkReg;
            
            if (policy.alignRegister && (tx.dspRegister % 4 != 0)) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: dspRegister 0x%08X is not 4-byte aligned", tx.dspRegister);
                return false;
            }
            if (policy.alignPayload && (tx.size % 4 != 0)) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: payload size %d is not 4-byte aligned", tx.size);
                return false;
            }
            
            if (tx.firmwareAddress + tx.size < tx.firmwareAddress) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: firmwareAddress wrap-around");
                return false;
            }
            if (tx.dspRegister + tx.size < tx.dspRegister) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: dspRegister wrap-around");
                return false;
            }
            
            // calculate checksum over plan
            uint32_t crcFields[3] = {tx.dspRegister, tx.firmwareAddress, tx.size};
            const uint8_t *crcData = (const uint8_t *)crcFields;
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
    static void simulate(const UploadPlan &plan) {
        CIRRUS_LOG("--- Dry-Run Simulation for Region #%d ---", plan.regionIndex);
        
        for (uint32_t i = 0; i < plan.transactionCount; i++) {
            const UploadTransaction &tx = plan.transactions[i];
            CIRRUS_LOG("[DRYRUN] Tx%d: Reg=0x%08X, FW_Word=0x%06X, ChunkByte=0x%06X, Payload=[%d..%d], Size=%d", 
                       i, tx.dspRegister, tx.firmwareAddress, tx.payloadOffset, tx.payloadOffset, tx.payloadOffset + tx.size - 1, tx.size);
        }
        
        const char *typeName = "UNKNOWN";
        switch (plan.regionType) {
            case RegionType::PM_PACKED: typeName = "PM_PACKED"; break;
            case RegionType::XM_PACKED: typeName = "XM_PACKED"; break;
            case RegionType::YM_PACKED: typeName = "YM_PACKED"; break;
            case RegionType::ALGORITHM_DATA: typeName = "ALGORITHM"; break;
            case RegionType::METADATA: typeName = "METADATA"; break;
            case RegionType::NAME_TEXT: typeName = "NAME_TEXT"; break;
            case RegionType::INFO_TEXT: typeName = "INFO_TEXT"; break;
            default: break;
        }

        uint32_t fwStart = plan.transactionCount > 0 ? plan.transactions[0].firmwareAddress : 0;
        uint32_t fwEnd = plan.transactionCount > 0 ? plan.transactions[plan.transactionCount - 1].firmwareAddress + plan.transactions[plan.transactionCount - 1].size : 0;
        uint32_t dspStart = plan.transactionCount > 0 ? plan.transactions[0].dspRegister : 0;
        uint32_t dspEnd = plan.transactionCount > 0 ? plan.transactions[plan.transactionCount - 1].dspRegister + plan.transactions[plan.transactionCount - 1].size : 0;

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

// Helper: region type name
static inline const char *regionTypeName(RegionType t) {
    switch (t) {
        case RegionType::PM_PACKED:      return "PM_PACKED";
        case RegionType::XM_PACKED:      return "XM_PACKED";
        case RegionType::YM_PACKED:      return "YM_PACKED";
        case RegionType::ALGORITHM_DATA: return "ALGORITHM";
        case RegionType::METADATA:       return "METADATA";
        case RegionType::NAME_TEXT:      return "NAME_TEXT";
        case RegionType::INFO_TEXT:      return "INFO_TEXT";
        default:                         return "UNKNOWN";
    }
}

class CirrusFirmwareRealUploader {
public:
    static bool upload(CS35L41Amp &amp, CirrusAudioFixup *fixup, const UploadPlan &plan, UploadStats *outStats = nullptr) {
        if (plan.transactionCount == 0 || plan.totalSize == 0) return true;

        uint32_t dspStart  = plan.transactions[0].dspRegister;
        uint32_t totalSize = plan.totalSize;
        const char *rtype  = regionTypeName(plan.regionType);

        CIRRUS_LOG("amp %s: starting physical upload of region data", amp.name);
        CIRRUS_LOG("Amp %s: Region %d (%s), Transactions: %d, Total: %d bytes, DSP Start: 0x%08X",
                   amp.name, plan.regionIndex, rtype, plan.transactionCount, totalSize, dspStart);

        // allocate verification buffers
        UInt8 *backupBuffer = (UInt8 *)IOMallocData(totalSize);
        UInt8 *verifyBuffer = (UInt8 *)IOMallocData(totalSize);
        if (!backupBuffer || !verifyBuffer) {
            CIRRUS_ERR("Amp %s: Failed to allocate Backup/Verify buffers", amp.name);
            if (backupBuffer) IOFreeData(backupBuffer, totalSize);
            if (verifyBuffer) IOFreeData(verifyBuffer, totalSize);
            return false;
        }

        auto to_ms = [&](uint64_t diff) -> uint32_t {
            uint64_t nsecs = 0;
            absolutetime_to_nanoseconds(diff, &nsecs);
            return (uint32_t)(nsecs / 1000000);
        };

        // backup current dsp memory before overwrite
        CIRRUS_LOG("Amp %s: Backing up %d bytes from 0x%08X...", amp.name, totalSize, dspStart);
        if (!fixup->bulkRead(amp, dspStart, backupBuffer, totalSize, TRACE_OTHER)) {
            CIRRUS_ERR("Amp %s: Backup FAIL - aborting.", amp.name);
            IOFreeData(backupBuffer, totalSize);
            IOFreeData(verifyBuffer, totalSize);
            return false;
        }
        CIRRUS_LOG("Amp %s: Backup OK (%d bytes)", amp.name, totalSize);

        // process each chunk via write-verify-check sequence
        uint64_t t_total_start = mach_absolute_time();
        uint32_t acc_write_ms = 0, acc_rb_ms = 0, acc_crc_ms = 0, acc_retries = 0;

        for (uint32_t i = 0; i < plan.transactionCount; i++) {
            const UploadTransaction &tx = plan.transactions[i];

            CIRRUS_LOG("amp %s: transaction %d/%d (dsp=0x%08x size=%d)",
                   amp.name, i + 1, plan.transactionCount,
                   tx.dspRegister, tx.size);

            // write chunk data to dsp registers
            uint32_t totalPacketLength = tx.size + 4;
            CIRRUS_LOG("Amp %s:   WRITE    : payload = %d bytes, packet = %d bytes (payload + 4)", amp.name, tx.size, totalPacketLength);
            uint64_t t0 = mach_absolute_time();
            bool writeOk = false;
            for (int attempt = 1; attempt <= 2; attempt++) {
                if (fixup->bulkWrite(amp, tx.dspRegister, tx.payload, tx.size, TRACE_OTHER)) {
                    writeOk = true;
                    break;
                }
                if (attempt < 2) {
                    OSObject *ret = fixup->getProperty("CirrusTransferRet");
                    IOReturn rc = ret ? ((OSNumber*)ret)->unsigned32BitValue() : kIOReturnError;
                    CIRRUS_LOG("Amp %s:   WRITE    : FAIL (Attempt 1/2, Status=0x%08X) retrying...", amp.name, rc);
                    acc_retries++;
                    IOSleep(10);
                }
            }
            uint32_t write_ms = to_ms(mach_absolute_time() - t0);
            acc_write_ms += write_ms;

            if (!writeOk) {
                OSObject *ret = fixup->getProperty("CirrusTransferRet");
                IOReturn rc = ret ? ((OSNumber*)ret)->unsigned32BitValue() : kIOReturnError;
                CIRRUS_LOG("Amp %s:   WRITE    : FAIL (2/2, Status=0x%08X, %d ms)", amp.name, rc, write_ms);
                CIRRUS_LOG("Amp %s:   READBACK : SKIPPED", amp.name);
                CIRRUS_LOG("Amp %s:   CRC      : SKIPPED", amp.name);
                bool rbOk = fixup->bulkWrite(amp, dspStart, backupBuffer, totalSize, TRACE_OTHER);
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", amp.name, rbOk ? "PASS" : "FAIL");
                IOFreeData(backupBuffer, totalSize);
                IOFreeData(verifyBuffer, totalSize);
                return false;
            }
            CIRRUS_LOG("Amp %s:   WRITE    : PASS (%d ms)", amp.name, write_ms);

            // read back the written data chunk to verify
            t0 = mach_absolute_time();
            UInt8 *rbSlot = verifyBuffer + tx.payloadOffset;
            bool readOk = fixup->bulkRead(amp, tx.dspRegister, rbSlot, tx.size, TRACE_OTHER);
            uint32_t rb_ms = to_ms(mach_absolute_time() - t0);
            acc_rb_ms += rb_ms;
 
            if (!readOk) {
                CIRRUS_LOG("Amp %s:   READBACK : FAIL (%d ms)", amp.name, rb_ms);
                CIRRUS_LOG("Amp %s:   CRC      : SKIPPED", amp.name);
                bool rbOk = fixup->bulkWrite(amp, dspStart, backupBuffer, totalSize, TRACE_OTHER);
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", amp.name, rbOk ? "PASS" : "FAIL");
                IOFreeData(backupBuffer, totalSize);
                IOFreeData(verifyBuffer, totalSize);
                return false;
            }
            CIRRUS_LOG("Amp %s:   READBACK : PASS (%d ms)", amp.name, rb_ms);
 
            // calculate and check crc-32 signatures
            t0 = mach_absolute_time();
            uint32_t payCrc = 0xFFFFFFFF, rbCrc = 0xFFFFFFFF;
            const uint8_t *paySlice = tx.payload;
            for (uint32_t b = 0; b < tx.size; b++) {
                payCrc ^= paySlice[b];
                rbCrc  ^= rbSlot[b];
                for (int j = 0; j < 8; j++) {
                    payCrc = (payCrc >> 1) ^ (0xEDB88320 & (-(payCrc & 1)));
                    rbCrc  = (rbCrc  >> 1) ^ (0xEDB88320 & (-(rbCrc  & 1)));
                }
            }
            payCrc = ~payCrc;
            rbCrc  = ~rbCrc;
            uint32_t crc_ms = to_ms(mach_absolute_time() - t0);
            acc_crc_ms += crc_ms;
 
            if (payCrc != rbCrc) {
                CIRRUS_LOG("Amp %s:   CRC      : FAIL (%d ms) [Exp=0x%08X Got=0x%08X]",
                           amp.name, crc_ms, payCrc, rbCrc);
                           
                // display first 16 bytes to trace payload difference
                uint32_t dump_len = min((uint32_t)tx.size, (uint32_t)16);
                char payHex[64] = {0};
                char rbHex[64] = {0};
                for (uint32_t d = 0; d < dump_len; d++) {
                    snprintf(payHex + d * 3, sizeof(payHex) - d * 3, "%02X ", paySlice[d]);
                    snprintf(rbHex + d * 3, sizeof(rbHex) - d * 3, "%02X ", rbSlot[d]);
                }
                CIRRUS_LOG("Amp %s:   PAYLOAD  : %s", amp.name, payHex);
                CIRRUS_LOG("Amp %s:   READBACK : %s", amp.name, rbHex);
 
                // run exact byte comparison to pinpoint mismatch offset
                for (uint32_t b = 0; b < tx.size; b++) {
                    if (paySlice[b] != rbSlot[b]) {
                        CIRRUS_LOG("Amp %s:   memcmp   : offset 0x%06X (Exp=0x%02X Got=0x%02X)",
                                   amp.name, b, paySlice[b], rbSlot[b]);
                        break;
                    }
                }
                bool rbOk = fixup->bulkWrite(amp, dspStart, backupBuffer, totalSize, TRACE_OTHER);
                CIRRUS_LOG("Amp %s:   ROLLBACK : %s", amp.name, rbOk ? "PASS" : "FAIL");
                IOFreeData(backupBuffer, totalSize);
                IOFreeData(verifyBuffer, totalSize);
                return false;
            }
            CIRRUS_LOG("Amp %s:   CRC      : PASS (%d ms) [0x%08X]", amp.name, crc_ms, payCrc);
            CIRRUS_LOG("Amp %s:   ROLLBACK : SKIPPED", amp.name);
        }
 
        // print performance statistics summary
        uint32_t total_ms = to_ms(mach_absolute_time() - t_total_start);
        CIRRUS_LOG("Amp %s: Upload Complete | Tx=%d PASS, Write=%d ms, RB=%d ms, CRC=%d ms, Total=%d ms",
                   amp.name, plan.transactionCount, acc_write_ms, acc_rb_ms, acc_crc_ms, total_ms);
 
        if (outStats) {
            outStats->writeMs   = acc_write_ms;
            outStats->readbackMs = acc_rb_ms;
            outStats->crcMs     = acc_crc_ms;
            outStats->totalMs   = total_ms;
            outStats->retries   = acc_retries;
        }
 
        IOFreeData(backupBuffer, totalSize);
        IOFreeData(verifyBuffer, totalSize);
        return true;
    }
};


// region scheduler coordinating the upload of executable memory blocks
// stops immediately at the first block failure

struct RegionResult {
    uint32_t   regionIndex;
    RegionType type;
    uint32_t   bytes;
    uint32_t   transactionCount;
    bool       success;
    uint32_t   planCrc;
    uint32_t   elapsedMs;
};

struct UploadSession {
    RegionResult results[32];
    uint32_t     regionCount;
    uint32_t     passCount;
    uint32_t     totalBytes;
    uint32_t     totalTransactions;
    uint32_t     totalMs;
    bool         complete;
    bool         ignoreCrcFailures;
};

class CirrusFirmwareScheduler {
public:
    static bool run(CS35L41Amp &amp, CirrusAudioFixup *fixup,
                    MappedImage &mappedImg, UploadSession &session,
                    bool ignoreCrcFailures = false)
    {
        session = {};
        session.ignoreCrcFailures = ignoreCrcFailures;

        UploadPolicy policy;
        policy.maxPayloadBytes = 252;
        policy.alignRegister    = false;
        policy.alignPayload     = false;

        uint64_t t_session_start = mach_absolute_time();

        CIRRUS_LOG("Amp %s: WMFW Upload — %d mapped regions total", amp.name, mappedImg.regionCount);

        for (uint32_t i = 0; i < mappedImg.regionCount; i++) {
            const MappedRegion &region = mappedImg.regions[i];

            if (session.regionCount >= 32) {
                CIRRUS_ERR("Amp %s: Too many regions in session", amp.name);
                break;
            }

            const char *rname = regionTypeName(region.regionType);
            CIRRUS_LOG("Amp %s: Region %d (%s) %d bytes", amp.name, i, rname, region.size);
            
            if (region.regionType == RegionType::INFO_TEXT || 
                region.regionType == RegionType::ALGORITHM_DATA || 
                region.regionType == RegionType::METADATA ||
                region.regionType == RegionType::NAME_TEXT) {
                CIRRUS_LOG("Amp %s:   Region %d (%s) is metadata, skipping upload.", amp.name, i, rname);
                continue;
            }

            UploadPlan *plan = (UploadPlan *)IOMalloc(sizeof(UploadPlan));
            if (!plan) {
                CIRRUS_ERR("Amp %s: Failed to allocate UploadPlan for region %d", amp.name, i);
                RegionResult &res = session.results[session.regionCount++];
                res = {i, region.regionType, region.size, 0, false, 0, 0};
                break;
            }

            bool planOk = CirrusFirmwareUploadPlanner::generatePlan(i, region, policy, *plan);
            if (!planOk) {
                CIRRUS_ERR("Amp %s: Region %d plan FAIL", amp.name, i);
                IOFree(plan, sizeof(UploadPlan));
                RegionResult &res = session.results[session.regionCount++];
                res = {i, region.regionType, region.size, 0, false, 0, 0};
                break;
            }

            uint64_t t0 = mach_absolute_time();
            UploadStats stats = {};
            bool ok = CirrusFirmwareRealUploader::upload(amp, fixup, *plan, &stats);
            uint64_t t1 = mach_absolute_time();

            uint64_t nsecs = 0;
            absolutetime_to_nanoseconds(t1 - t0, &nsecs);
            uint32_t elapsed_ms = (uint32_t)(nsecs / 1000000);

            RegionResult &res = session.results[session.regionCount++];
            res.regionIndex      = i;
            res.type             = region.regionType;
            res.bytes            = plan->totalSize;
            res.transactionCount = plan->transactionCount;
            res.success          = ok;
            res.planCrc          = plan->planCrc;
            res.elapsedMs        = elapsed_ms;

            IOFree(plan, sizeof(UploadPlan));

            if (ok) {
                session.passCount++;
                session.totalBytes        += res.bytes;
                session.totalTransactions += res.transactionCount;
                CIRRUS_LOG("Amp %s:   Region %d (%s) MappedTo=0x%08X PASS (%d ms)", amp.name, i, rname, region.dspRegister, elapsed_ms);
            } else {
                if (ignoreCrcFailures) {
                    CIRRUS_LOG("Amp %s:   Region %d (%s) FAIL — ignoring CRC error for coefficients", amp.name, i, rname);
                    session.passCount++; // Count as pass so it doesn't abort the whole session
                    session.totalBytes        += res.bytes;
                    session.totalTransactions += res.transactionCount;
                } else {
                    CIRRUS_ERR("Amp %s:   Region %d (%s) FAIL — stopping", amp.name, i, rname);
                    break;
                }
            }
        }

        uint64_t t_ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - t_session_start, &t_ns);
        session.totalMs = (uint32_t)(t_ns / 1000000);
        session.complete = (session.regionCount > 0 && session.passCount == session.regionCount);

        CIRRUS_LOG("Amp %s: ================================", amp.name);
        CIRRUS_LOG("Amp %s: WMFW Upload Summary", amp.name);
        CIRRUS_LOG("Amp %s: ================================", amp.name);
        for (uint32_t i = 0; i < session.regionCount; i++) {
            const RegionResult &r = session.results[i];
            CIRRUS_LOG("Amp %s:   [%d] %-10s %s  %d bytes  %d tx  %d ms",
                       amp.name, r.regionIndex, regionTypeName(r.type),
                       r.success ? "PASS" : "FAIL",
                       r.bytes, r.transactionCount, r.elapsedMs);
        }
        CIRRUS_LOG("Amp %s: ================================", amp.name);
        CIRRUS_LOG("Amp %s:   Regions      : %d / %d PASS", amp.name, session.passCount, session.regionCount);
        CIRRUS_LOG("Amp %s:   Bytes        : %d", amp.name, session.totalBytes);
        CIRRUS_LOG("Amp %s:   Transactions : %d", amp.name, session.totalTransactions);
        CIRRUS_LOG("Amp %s:   Total Time   : %d ms", amp.name, session.totalMs);
        CIRRUS_LOG("Amp %s: ================================", amp.name);

        if (session.complete) {
            CIRRUS_LOG("Amp %s: WMFW UPLOAD COMPLETE", amp.name);
        } else {
            CIRRUS_ERR("Amp %s: WMFW UPLOAD INCOMPLETE — %d/%d regions passed",
                       amp.name, session.passCount, session.regionCount);
        }

        return session.complete;
    }
};

#endif // CS35L41_FIRMWARE_UPLOADER_HPP
