#ifndef CS35L41_FIRMWARE_PARSER_HPP
#define CS35L41_FIRMWARE_PARSER_HPP

#include <IOKit/IOLib.h>

#define WMFW_MAGIC_0 'W'
#define WMFW_MAGIC_1 'M'
#define WMFW_MAGIC_2 'F'
#define WMFW_MAGIC_3 'W'

#define WMFW_ABSOLUTE         0xf0
#define WMFW_ALGORITHM_DATA   0xf2
#define WMFW_METADATA         0xfc
#define WMFW_NAME_TEXT        0xfe
#define WMFW_INFO_TEXT        0xff

#define WMFW_HALO_PM_PACKED 0x10
#define WMFW_HALO_XM_PACKED 0x11
#define WMFW_HALO_YM_PACKED 0x12
#define WMFW_ADSP2_XM       0x05
#define WMFW_ADSP2_YM       0x06

// Standard Linux structures
#pragma pack(push, 1)

struct wmfw_header {
    char magic[4];
    uint32_t len;
    uint16_t rev;
    uint8_t core;
    uint8_t ver;
};

struct wmfw_adsp2_sizes {
    uint32_t xm;
    uint32_t ym;
    uint32_t pm;
    uint32_t zm;
};

struct wmfw_footer {
    uint64_t timestamp;
    uint32_t checksum;
};

struct wmfw_region {
    union {
        uint32_t type_be;
        uint32_t offset_le;
    };
    uint32_t len;
    uint8_t data[];
};

#pragma pack(pop)

#define MAX_FIRMWARE_REGIONS 32

enum class RegionType : uint32_t {
    PM_PACKED = 0x10,
    XM_PACKED = 0x11,
    YM_PACKED = 0x12,
    ALGORITHM_DATA = 0xF2,
    METADATA = 0xFC,
    NAME_TEXT = 0xFE,
    INFO_TEXT = 0xFF,
    UNKNOWN = 0xFFFF
};

enum class MappingStatus {
    OK,
    UnsupportedRegion,
    Overflow,
    AlignmentError,
    InvalidOffset
};

// Intermediate Representation
struct FirmwareSpan {
    const uint8_t *begin;
    uint32_t size;
};

struct FirmwareRegion {
    RegionType regionType;
    uint32_t   baseWordOffset;
    const uint8_t* data;
    uint32_t   length;
};

struct AlgorithmInfo {
    uint32_t   id;
    RegionType region;
    uint32_t   baseWordOffset;
    uint32_t   size;
};

struct CoefficientBlock {
    uint32_t id;
    uint16_t type;
    uint32_t offset;
    uint32_t length;
    uint32_t payloadCrc;
    const uint8_t* data;
};

struct FirmwareImage {
    uint32_t fw_magic;
    uint32_t fw_version;
    uint32_t fw_total_bytes;
    uint32_t fw_crc;
    uint8_t fw_core;
    uint16_t fw_core_rev;
    uint32_t fw_id; // Firmware ID from Halo ID Header
    
    FirmwareRegion regions[MAX_FIRMWARE_REGIONS];
    uint32_t regionCount;
    
    AlgorithmInfo algorithms[32];
    uint32_t algorithmCount;
    
    CoefficientBlock coefficients[128];
    uint32_t coefficientCount;
    uint32_t total_coeff_payload_bytes;
    
    // Stats
    uint32_t stat_xm_blocks;
    uint32_t stat_ym_blocks;
    uint32_t stat_pm_blocks;
    uint32_t stat_coeff_blocks;
    uint32_t stat_metadata_blocks;
    uint32_t stat_unknown_blocks;
    
    // Calculated Fingerprint
    uint32_t fingerprint;
};

// Phase 5C.1: Mapper Structures

struct MappedRegion {
    RegionType regionType;
    uint32_t firmwareAddress;
    uint32_t dspRegister;
    uint32_t size;
    FirmwareSpan data;
};

struct MappedImage {
    MappedRegion regions[MAX_FIRMWARE_REGIONS];
    uint32_t regionCount;
    uint32_t mappingCrc;
};

class CirrusFirmwareMapper {
public:
    static MappingStatus mapPackedAddress(RegionType type, uint32_t wordOffset, uint32_t byteOffset, uint32_t &regAddress) {
        switch (type) {
            case RegionType::PM_PACKED:
                regAddress = 0x03800000 + (wordOffset * 5) + byteOffset;
                break;
            case RegionType::XM_PACKED:
                regAddress = 0x02000000 + (wordOffset * 3) + byteOffset;
                break;
            case RegionType::YM_PACKED:
                regAddress = 0x02C00000 + (wordOffset * 3) + byteOffset;
                break;
            default:
                return MappingStatus::UnsupportedRegion;
        }
        
        return MappingStatus::OK;
    }

    static bool mapFirmwareImage(const FirmwareImage &image, MappedImage &outMapped) {
        outMapped.regionCount = 0;
        outMapped.mappingCrc = 0xFFFFFFFF;
        
        for (uint32_t i = 0; i < image.regionCount; i++) {
            const FirmwareRegion &inReg = image.regions[i];
            MappedRegion &outReg = outMapped.regions[outMapped.regionCount];
            
            // Map Type
            outReg.regionType = inReg.regionType;
            
            outReg.firmwareAddress = inReg.baseWordOffset;
            outReg.size = inReg.length;
            outReg.data.begin = inReg.data;
            outReg.data.size = inReg.length;
            outReg.dspRegister = 0; // default for unmapped
            
            if (outReg.regionType == RegionType::PM_PACKED || 
                outReg.regionType == RegionType::XM_PACKED || 
                outReg.regionType == RegionType::YM_PACKED) {
                
                MappingStatus status = mapPackedAddress(outReg.regionType, outReg.firmwareAddress, 0, outReg.dspRegister);
                if (status != MappingStatus::OK) {
                    CIRRUS_ERR("Mapping failed for region %d: status %d", i, (int)status);
                    return false;
                }
            } else {
                // Non-memory region (algorithms, metadata)
                outReg.dspRegister = 0xFFFFFFFF; // Not applicable
            }
            
            // Cumulate CRC
            const uint8_t *crcData = (const uint8_t *)&outReg;
            // Calculate CRC over the metadata: regionType(4), firmwareAddress(4), dspRegister(4), size(4)
            for (size_t k = 0; k < 16; k++) {
                outMapped.mappingCrc ^= crcData[k];
                for (size_t j = 0; j < 8; j++) {
                    outMapped.mappingCrc = (outMapped.mappingCrc >> 1) ^ (0xEDB88320 & (-(outMapped.mappingCrc & 1)));
                }
            }
            
            outMapped.regionCount++;
        }
        
        outMapped.mappingCrc = ~outMapped.mappingCrc;
        return true;
    }
};

class CirrusFirmwareParser {
public:
    static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++) {
            crc ^= data[i];
            for (size_t j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return ~crc;
    }

    static bool validateWMFW(const uint8_t *data, size_t size) {
        if (size < sizeof(wmfw_header)) {
            CIRRUS_ERR("WMFW file too small for header (%zu bytes)", size);
            return false;
        }
        
        const wmfw_header *header = (const wmfw_header *)data;
        if (header->magic[0] != WMFW_MAGIC_0 || header->magic[1] != WMFW_MAGIC_1 ||
            header->magic[2] != WMFW_MAGIC_2 || header->magic[3] != WMFW_MAGIC_3) {
            CIRRUS_ERR("WMFW invalid magic");
            return false;
        }
        
        // sizes and footer
        size_t pos = sizeof(wmfw_header);
        if (size < pos + sizeof(wmfw_adsp2_sizes)) {
            CIRRUS_ERR("WMFW truncated at sizes block");
            return false;
        }
        pos += sizeof(wmfw_adsp2_sizes);
        
        if (size < pos + sizeof(wmfw_footer)) {
            CIRRUS_ERR("WMFW truncated at footer");
            return false;
        }
        pos += sizeof(wmfw_footer);
        
        // Pass 1: Bound Check Only
        while (pos < size) {
            if (size - pos < sizeof(wmfw_region)) {
                CIRRUS_ERR("WMFW region header out of bounds at offset %zu", pos);
                return false;
            }
            const wmfw_region *region = (const wmfw_region *)&data[pos];
            uint32_t len = OSSwapLittleToHostInt32(region->len);
            
            if (size - pos - sizeof(wmfw_region) < len) {
                CIRRUS_ERR("WMFW region data out of bounds at offset %zu (len %u)", pos, len);
                return false;
            }
            
            pos += sizeof(wmfw_region) + len;
        }
        
        if (pos != size) {
            CIRRUS_LOG("WMFW Warning: Expected %zu bytes, parsed %zu bytes", size, pos);
        }
        
        return true;
    }

    static bool parseWMFW(const uint8_t *data, size_t size, FirmwareImage *outImage) {
        if (!outImage) return false;
        memset(outImage, 0, sizeof(FirmwareImage));
        
        if (!validateWMFW(data, size)) {
            return false;
        }
        
        const wmfw_header *header = (const wmfw_header *)data;
        outImage->fw_magic = (header->magic[0] << 24) | (header->magic[1] << 16) | (header->magic[2] << 8) | header->magic[3];
        outImage->fw_version = header->ver;
        outImage->fw_total_bytes = (uint32_t)size;
        outImage->fw_crc = calculate_crc32(data, size);
        outImage->fw_core = header->core;
        outImage->fw_core_rev = header->rev;
        
        size_t pos = sizeof(wmfw_header) + sizeof(wmfw_adsp2_sizes) + sizeof(wmfw_footer);
        
        while (pos < size && outImage->regionCount < 32) {
            const wmfw_region *raw_region = (const wmfw_region *)&data[pos];
            
            uint32_t type = OSSwapBigToHostInt32(raw_region->type_be) & 0xFF;
            uint32_t offset = OSSwapLittleToHostInt32(raw_region->offset_le) & 0xFFFFFF;
            uint32_t len = OSSwapLittleToHostInt32(raw_region->len);
            
            FirmwareRegion &reg = outImage->regions[outImage->regionCount++];
            reg.baseWordOffset = offset;
            reg.data = raw_region->data;
            reg.length = len;
            
            switch (type) {
                case WMFW_HALO_XM_PACKED: reg.regionType = RegionType::XM_PACKED; outImage->stat_xm_blocks++; break;
                case WMFW_HALO_YM_PACKED: reg.regionType = RegionType::YM_PACKED; outImage->stat_ym_blocks++; break;
                case WMFW_HALO_PM_PACKED: reg.regionType = RegionType::PM_PACKED; outImage->stat_pm_blocks++; break;
                case WMFW_ALGORITHM_DATA: reg.regionType = RegionType::ALGORITHM_DATA; break;
                case WMFW_METADATA:       reg.regionType = RegionType::METADATA; break;
                case WMFW_INFO_TEXT:      reg.regionType = RegionType::INFO_TEXT; break;
                case WMFW_NAME_TEXT:      reg.regionType = RegionType::NAME_TEXT; break;
                case WMFW_ABSOLUTE:       reg.regionType = RegionType::UNKNOWN; break;
                default:                  reg.regionType = RegionType::UNKNOWN; break;
            }
            
            // Extract Algorithm Table from the first XM_PACKED region
            if (type == WMFW_HALO_XM_PACKED && offset == 0 && outImage->algorithmCount == 0 && len >= 30) {
                extractHaloAlgorithmsFromXM(outImage, reg);
            }
            
            pos += sizeof(wmfw_region) + len;
        }
        
        return true;
    }

    // Note: This is a Halo firmware specific extraction. It relies on the DSP memory layout 
    // where wmfw_halo_id_hdr is placed at offset 0 of the XM_PACKED region.
    static void extractHaloAlgorithmsFromXM(FirmwareImage *outImage, const FirmwareRegion &reg) {
        if (reg.length >= 12) {
            uint32_t fw_id = (reg.data[2*3] << 16) | (reg.data[2*3+1] << 8) | reg.data[2*3+2];
            outImage->fw_id = fw_id;
        }

        // wmfw_halo_id_hdr has 10 32-bit words, stored as 3 bytes each
        uint32_t n_algs = (reg.data[9*3] << 16) | (reg.data[9*3+1] << 8) | reg.data[9*3+2];
        
        if (n_algs > 0 && n_algs < 32 && reg.length >= 30 + (n_algs * 18)) {
            for (uint32_t i = 0; i < n_algs; i++) {
                size_t alg_pos = 30 + (i * 18);
                uint32_t alg_id = (reg.data[alg_pos] << 16) | (reg.data[alg_pos+1] << 8) | reg.data[alg_pos+2];
                // wmfw_halo_alg_hdr: id(0), ver(1), xm_base(2), xm_size(3), ym_base(4), ym_size(5)
                uint32_t xm_base = (reg.data[alg_pos+6] << 16) | (reg.data[alg_pos+7] << 8) | reg.data[alg_pos+8];
                uint32_t ym_base = (reg.data[alg_pos+12] << 16) | (reg.data[alg_pos+13] << 8) | reg.data[alg_pos+14];
                
                AlgorithmInfo &alg = outImage->algorithms[outImage->algorithmCount++];
                alg.id = alg_id;
                alg.baseWordOffset = xm_base;
                alg.region = RegionType::XM_PACKED; // Defaulting to XM base for tracking
                
                CIRRUS_LOG("Algorithm %u:\n  ID        = 0x%08X\n  Region    = XM\n  Base      = 0x%08X", 
                           outImage->algorithmCount - 1, alg.id, alg.baseWordOffset);
            }
        }
    }

    static bool parseBIN(const uint8_t *data, size_t size, FirmwareImage *outImage) {
        if (!outImage) return false;
        
        // Header is wmfw_coeff_hdr: magic(4), len(4), rev(4), core(4)
        if (size < 16) {
            CIRRUS_ERR("BIN file too small for header");
            return false;
        }
        
        if (data[0] != 'W' || data[1] != 'M' || data[2] != 'D' || data[3] != 'R') {
            CIRRUS_ERR("BIN file invalid magic (expected WMDR)");
            return false;
        }
        
        uint32_t pos = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24); // len is Little Endian
        
        while (pos < size && outImage->coefficientCount < 128) {
            if (size - pos < 16) { // wmfw_coeff_item is 16 bytes
                break;
            }
            
            CoefficientBlock &coeff = outImage->coefficients[outImage->coefficientCount++];
            
            coeff.offset = data[pos+0] | (data[pos+1] << 8); // offset is le16
            coeff.type = data[pos+2] | (data[pos+3] << 8); // type is le16
            coeff.id = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24); // id is le32
            // Skip ver(4) and sr(4)
            coeff.length = data[pos+12] | (data[pos+13] << 8) | (data[pos+14] << 16) | (data[pos+15] << 24); // len is le32
            
            pos += 16;
            coeff.data = &data[pos];
            coeff.payloadCrc = calculate_crc32(coeff.data, coeff.length);
            
            uint32_t type_masked = coeff.type & 0xFF; // strip extended flags
            
            outImage->total_coeff_payload_bytes += coeff.length;
            
            CIRRUS_LOG("Coeff Block %u\n  ID      = %u\n  Type    = 0x%X\n  Offset  = 0x%X\n  Length  = %u\n  CRC     = 0x%08X", 
                       outImage->coefficientCount - 1, coeff.id, type_masked, coeff.offset, coeff.length, coeff.payloadCrc);
            
            pos += (coeff.length + 3) & ~0x03; // 4-byte padding
        }
        
        return true;
    }
};

#endif
