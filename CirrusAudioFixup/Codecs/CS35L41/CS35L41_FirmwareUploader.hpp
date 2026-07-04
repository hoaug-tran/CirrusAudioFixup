#ifndef CS35L41_FIRMWARE_UPLOADER_HPP
#define CS35L41_FIRMWARE_UPLOADER_HPP

#include "CS35L41_FirmwareParser.hpp"
#include "../../CirrusAudioFixup.hpp"

#define MAX_UPLOAD_TRANSACTIONS 1024

struct UploadPolicy {
    uint32_t maxTransferBytes;
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
        
        if (policy.maxTransferBytes == 0) {
            CIRRUS_ERR("UploadPolicy maxTransferBytes cannot be 0");
            return false;
        }

        uint32_t remaining = region.size;
        uint32_t currentOffset = 0;
        
        while (remaining > 0) {
            if (outPlan.transactionCount >= MAX_UPLOAD_TRANSACTIONS) {
                CIRRUS_ERR("UPLOAD_PLAN_INVALID: Too many transactions");
                return false;
            }
            
            uint32_t chunkSize = (remaining > policy.maxTransferBytes) ? policy.maxTransferBytes : remaining;
            
            UploadTransaction &tx = outPlan.transactions[outPlan.transactionCount];
            tx.firmwareAddress = region.firmwareAddress + currentOffset;
            tx.payloadOffset = currentOffset;
            tx.payload = region.data.begin + currentOffset;
            tx.size = chunkSize;
            
            // Calculate register increment based on Region Type
            uint32_t chunkReg = 0;
            MappingStatus status = CirrusFirmwareMapper::mapPackedAddress(region.regionType, tx.firmwareAddress, tx.size, chunkReg);
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
            
            // Cumulate CRC over plan
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
            CIRRUS_LOG("[DRYRUN] Tx%d: Reg=0x%08X, FW=0x%06X, Offset=%d, Size=%d", 
                       i, tx.dspRegister, tx.firmwareAddress, tx.payloadOffset, tx.size);
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

#endif // CS35L41_FIRMWARE_UPLOADER_HPP
