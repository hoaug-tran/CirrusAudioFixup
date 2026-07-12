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
    uint32_t type_offset_le;
    uint32_t len;
    uint8_t data[];
};

#pragma pack(pop)

#define MAX_FIRMWARE_REGIONS 32
#define MAX_MAPPED_REGIONS   192  // WMFW regions(~8) + coefficient blocks(up to 128) + headroom

// Halo ID Header layout (24-bit packed words in XM memory)
static constexpr uint32_t kHaloFwIdWord          = 2;  // fw_id is word 2
static constexpr uint32_t kHaloVersionWord       = 3;  // version is word 3
static constexpr uint32_t kHaloVendorIdWord      = 4;  // vendor_id is word 4
static constexpr uint32_t kHaloIdHdrWords        = 9;  // Header is 9 words
static constexpr uint32_t kHaloAlgTableStartWord = 9;  // Algorithm table starts immediately at word 9
static constexpr uint32_t kHaloAlgEntryWords     = 5;  // 5 words per algorithm (id, ver, xm_base, ym_base, pm_base)

// Helper: read a 24-bit packed big-endian word from packed XM memory
static inline uint32_t readPacked24BE(const uint8_t *data, uint32_t wordIdx) {
    const uint32_t b = wordIdx * 3;
    return ((uint32_t)data[b] << 16) | ((uint32_t)data[b+1] << 8) | (uint32_t)data[b+2];
}

enum class RegionType : uint32_t {
    PM_PACKED = 0x10,
    XM_PACKED = 0x11,
    YM_PACKED = 0x12,
    XM_UNPACKED = 0x5,
    YM_UNPACKED = 0x6,
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
    uint32_t   baseWordOffset; // xm_base
    uint32_t   ymBaseWordOffset; // ym_base
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

// WMFW Metadata Cache Structures
struct WMFWControl {
    uint16_t offset;
    uint16_t type;
    uint32_t size;
    char name[64];
    uint16_t ctl_type;
    uint16_t flags;
    uint32_t len;
};

struct WMFWAlgorithm {
    uint32_t id;
    char name[64];
    uint32_t firstControl;
    uint32_t controlCount;
};

struct WMFWControlRef {
    const WMFWAlgorithm *algorithm;
    const WMFWControl *control;
};


struct FirmwareImage {
    uint32_t fw_magic;
    uint32_t fw_version;
    uint32_t fw_total_bytes;
    uint32_t fw_crc;
    uint32_t fw_core;
    uint32_t fw_core_rev;
    
    // Halo specific
    uint32_t fw_id;
    uint32_t n_algs;
    uint32_t xm_dump_crc;
    
    // Extracted Algorithm Headers from UNPACK24_0 (DSP RAM)
    uint32_t algorithmCount;
    AlgorithmInfo algorithms[32];
    
    // Specific algorithm match (usually CSPL = 205)
    uint32_t algorithm_id;
    uint32_t algorithm_version;
    uint32_t algorithm_xm_base;
    uint32_t algorithm_xm_size;
    uint32_t algorithm_ym_base;
    uint32_t algorithm_ym_size;
    
    // WMFW Metadata Cache
    uint32_t wmfwAlgorithmCount;
    WMFWAlgorithm wmfwAlgorithms[8]; // Max 8 algorithms in metadata
    
    uint32_t wmfwControlCount;
    WMFWControl wmfwControls[512]; // Global pool for controls
    
    uint32_t regionCount;
    FirmwareRegion regions[32];
    
    uint32_t coefficientCount;
    CoefficientBlock coefficients[128];
    uint32_t total_coeff_payload_bytes;
    
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
    MappedRegion regions[MAX_MAPPED_REGIONS]; // 32 firmware + up to 128 coefficient blocks
    uint32_t regionCount;
    uint32_t mappingCrc;
};

struct HaloMemoryPointer {
    uint8_t region;
    uint16_t wordOffset;
};

inline HaloMemoryPointer decodePointer(uint32_t value) {
    return {
        static_cast<uint8_t>(value >> 16),
        static_cast<uint16_t>(value & 0xFFFF)
    };
}

class CirrusFirmwareMapper {
public:
    static MappingStatus mapPackedAddress(RegionType type, uint32_t wordOffset, uint32_t byteOffset, uint32_t &regAddress) {
        switch (type) {
            case RegionType::PM_PACKED:
                regAddress = 0x03800000 + (wordOffset * 5) + byteOffset;
                break;
            case RegionType::XM_PACKED:
                regAddress = ((0x02000000 + (wordOffset * 3)) & ~0x3) + byteOffset;
                break;
            case RegionType::YM_PACKED:
                regAddress = ((0x02C00000 + (wordOffset * 3)) & ~0x3) + byteOffset;
                break;
            case RegionType::XM_UNPACKED:
                regAddress = 0x02800000 + (wordOffset * 4) + byteOffset;
                break;
            case RegionType::YM_UNPACKED:
                regAddress = 0x03400000 + (wordOffset * 4) + byteOffset;
                break;
            default:
                return MappingStatus::UnsupportedRegion;
        }
        
        CIRRUS_LOG("mapPackedAddress: region=%u fwWordOffset=0x%08X byteOffset=%u -> dspReg=0x%08X", (uint16_t)type, wordOffset, byteOffset, regAddress);

        return MappingStatus::OK;
    }

    static bool mapFirmwareImage(const FirmwareImage &image, MappedImage &outMapped) {
        outMapped.regionCount = 0;
        outMapped.mappingCrc = 0xFFFFFFFF;
        
        for (uint32_t i = 0; i < image.regionCount; i++) {
            if (outMapped.regionCount >= MAX_MAPPED_REGIONS) {
                CIRRUS_ERR("mapFirmwareImage: MAX_MAPPED_REGIONS overflow at region %d", i);
                return false;
            }
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
                outReg.regionType == RegionType::YM_PACKED ||
                outReg.regionType == RegionType::XM_UNPACKED ||
                outReg.regionType == RegionType::YM_UNPACKED) {
                
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

    static bool mapCoefficients(const FirmwareImage &image, MappedImage &outMapped) {
        outMapped.regionCount = 0;
        outMapped.mappingCrc = 0xFFFFFFFF;
        
        for (uint32_t i = 0; i < image.coefficientCount; i++) {
            if (outMapped.regionCount >= MAX_MAPPED_REGIONS) {
                CIRRUS_ERR("mapCoefficients: MAX_MAPPED_REGIONS overflow");
                return false;
            }
            const CoefficientBlock &coeff = image.coefficients[i];
            
            // Find algorithm
            bool found = false;
            uint32_t alg_xm_base = 0;
            uint32_t alg_ym_base = 0;
            
            if (coeff.id == image.fw_id) {
                // Global coefficient (usually targets XM or YM directly or has special handling)
                CIRRUS_LOG("Global Coefficient %u (ID=0x%06X) mapping is not fully supported yet", i, coeff.id);
                continue;
            } else {
                for (uint32_t a = 0; a < image.algorithmCount; a++) {
                    if (image.algorithms[a].id == coeff.id) {
                        alg_xm_base = decodePointer(image.algorithms[a].baseWordOffset).wordOffset;
                        alg_ym_base = decodePointer(image.algorithms[a].ymBaseWordOffset).wordOffset;
                        CIRRUS_LOG("Coeff %u (ID=0x%06X) matched Algorithm %u (xm_base=0x%08X, ym_base=0x%08X)", i, coeff.id, a, alg_xm_base, alg_ym_base);
                        found = true;
                        break;
                    }
                }
            }
            
            if (!found) {
                CIRRUS_LOG("Skipping orphan coefficient block %u (ID=0x%06X)", i, coeff.id);
                continue;
            }
            
            MappedRegion &outReg = outMapped.regions[outMapped.regionCount];
            uint32_t type_masked = coeff.type & 0xFF;
            
            // For CS35L41, the WMFW types for packed memory are used.
            // WMFW_HALO_XM_PACKED = 0x11, YM_PACKED = 0x12, XM_UNPACKED = 0x5, YM_UNPACKED = 0x6
            if (type_masked == WMFW_HALO_XM_PACKED || type_masked == 0x5) {
                outReg.regionType = (type_masked == 0x5) ? RegionType::XM_UNPACKED : RegionType::XM_PACKED;
                outReg.firmwareAddress = alg_xm_base + coeff.offset;
                MappingStatus status = mapPackedAddress(outReg.regionType, outReg.firmwareAddress, 0, outReg.dspRegister);
                if (status != MappingStatus::OK) return false;
            } else if (type_masked == WMFW_HALO_YM_PACKED || type_masked == 0x6) {
                outReg.regionType = (type_masked == 0x6) ? RegionType::YM_UNPACKED : RegionType::YM_PACKED;
                outReg.firmwareAddress = alg_ym_base + coeff.offset;
                MappingStatus status = mapPackedAddress(outReg.regionType, outReg.firmwareAddress, 0, outReg.dspRegister);
                if (status != MappingStatus::OK) return false;
            } else {
                CIRRUS_LOG("Unsupported Coefficient Type 0x%X", type_masked);
                continue;
            }
            
            outReg.size = coeff.length;
            outReg.data.begin = coeff.data;
            outReg.data.size = coeff.length;
            
            // Cumulate CRC
            const uint8_t *crcData = (const uint8_t *)&outReg;
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

    static inline uint32_t readLE32(const uint8_t *p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }
    static inline uint16_t readLE16(const uint8_t *p) {
        return p[0] | (p[1] << 8);
    }

    static inline uint32_t alignStringLen(uint32_t str_len, uint32_t field_bytes) {
        return ((str_len + field_bytes) + 3) & ~0x03;
    }

    static uint32_t regionToReg(uint16_t type, uint32_t dspWord) {
        switch (type) {
            case WMFW_ADSP2_XM:
                return 0x02800000 + (dspWord * 4);
            case WMFW_ADSP2_YM:
                return 0x02C00000 + (dspWord * 4);
            case WMFW_HALO_XM_PACKED:
                return (0x02000000 + (dspWord * 3)) & ~0x3;
            case WMFW_HALO_YM_PACKED:
                return (0x02C00000 + (dspWord * 3)) & ~0x3;
            case WMFW_HALO_PM_PACKED:
                return 0x03800000 + (dspWord * 5); // Assuming PM uses the same base window or similar, though Linux driver does this
            default:
                CIRRUS_ERR("regionToReg: Unknown memory region type %u", type);
                return dspWord;
        }
    }

    static bool findControl(const FirmwareImage *fw, const char *name, WMFWControlRef &out) {
        for (uint32_t i = 0; i < fw->wmfwAlgorithmCount; i++) {
            const WMFWAlgorithm &alg = fw->wmfwAlgorithms[i];
            for (uint32_t j = 0; j < alg.controlCount; j++) {
                const WMFWControl &ctl = fw->wmfwControls[alg.firstControl + j];
                if (strncmp(ctl.name, name, sizeof(ctl.name)) == 0) {
                    out.algorithm = &alg;
                    out.control = &ctl;
                    return true;
                }
            }
        }
        return false;
    }

    static void parseWMFWAlgorithmData(const uint8_t *data, size_t size, FirmwareImage *outImage, size_t file_offset, uint8_t fw_version) {
        if (size < 4) return;
        
        if (fw_version < 2) {
            CIRRUS_LOG("Warning: This parser is designed for wmfw_ver >= 2. Skipping.");
            return;
        }
        
        uint32_t pos = 0;
        
        if (pos + 4 > size) return;
        uint32_t alg_id = readLE32(&data[pos]);
        pos += 4;
        
        // Algorithm Name (uint8)
        if (pos + 1 > size) return;
        uint8_t alg_name_len = data[pos];
        if (pos + alignStringLen(alg_name_len, 1) > size) return;
        char alg_name[256] = {0};
        memcpy(alg_name, &data[pos + 1], alg_name_len);
        pos += alignStringLen(alg_name_len, 1);
        
        // Algorithm Description (uint16)
        if (pos + 2 > size) return;
        uint16_t alg_desc_len = readLE16(&data[pos]);
        if (pos + alignStringLen(alg_desc_len, 2) > size) return;
        pos += alignStringLen(alg_desc_len, 2);
        
        if (pos + 4 > size) return;
        uint32_t ncoeff = readLE32(&data[pos]);
        pos += 4;
        
        CIRRUS_LOG("Algorithm:");
        CIRRUS_LOG("  id      = %u (0x%08X)", alg_id, alg_id);
        CIRRUS_LOG("  name    = %s", alg_name);
        CIRRUS_LOG("  coeffs  = %u", ncoeff);
        
        if (outImage->wmfwAlgorithmCount >= 8) {
            CIRRUS_LOG("Warning: Maximum algorithms reached.");
            return;
        }
        
        WMFWAlgorithm &alg = outImage->wmfwAlgorithms[outImage->wmfwAlgorithmCount++];
        alg.id = alg_id;
        strlcpy(alg.name, alg_name, sizeof(alg.name));
        alg.firstControl = outImage->wmfwControlCount;
        alg.controlCount = 0;
        
        for (uint32_t i = 0; i < ncoeff; i++) {
            if (pos + 8 > size) {
                CIRRUS_LOG("Sanity Check Failed: pos (0x%04X) exceeds payload size", pos);
                break;
            }
            
            uint16_t c_offset = readLE16(&data[pos]);
            uint16_t c_type = readLE16(&data[pos + 2]);
            uint32_t c_size = readLE32(&data[pos + 4]);
            
            uint32_t payload_start = pos + 8;
            uint32_t coeff_end = payload_start + c_size;
            
            if (c_size < 8) {
                CIRRUS_LOG("Sanity Check Failed: Coeff[%u] c_size (%u) is suspiciously small", i, c_size);
                break;
            }
            if (coeff_end > size) {
                CIRRUS_LOG("Sanity Check Failed: Coeff[%u] c_size (%u) exceeds payload", i, c_size);
                break;
            }
            
            uint32_t inner_pos = payload_start;
            
            // String 1: Coefficient Name (uint8)
            if (inner_pos + 1 > coeff_end) break;
            uint8_t c_name_len = data[inner_pos];
            if (inner_pos + alignStringLen(c_name_len, 1) > coeff_end) break;
            char c_name[256] = {0};
            memcpy(c_name, &data[inner_pos + 1], c_name_len);
            inner_pos += alignStringLen(c_name_len, 1);
            
            // String 2: Coefficient Description (uint8)
            if (inner_pos + 1 > coeff_end) break;
            uint8_t c_desc_len = data[inner_pos];
            if (inner_pos + alignStringLen(c_desc_len, 1) > coeff_end) break;
            inner_pos += alignStringLen(c_desc_len, 1);
            
            // String 3: Unknown String (uint16)
            if (inner_pos + 2 > coeff_end) break;
            uint16_t c_unknown_len = readLE16(&data[inner_pos]);
            if (inner_pos + alignStringLen(c_unknown_len, 2) > coeff_end) break;
            inner_pos += alignStringLen(c_unknown_len, 2);
            
            // Struct fields: ctl_type, flags, len
            if (inner_pos + 8 > coeff_end) break;
            uint16_t c_ctl_type = readLE16(&data[inner_pos]);
            uint16_t c_flags = readLE16(&data[inner_pos + 2]);
            uint32_t c_len = readLE32(&data[inner_pos + 4]);
            
            CIRRUS_LOG("  Coeff[%u] %s", i, c_name);
            CIRRUS_LOG("    offset   = 0x%04X", c_offset);
            CIRRUS_LOG("    type     = 0x%04X", c_type);
            CIRRUS_LOG("    ctl_type = 0x%04X", c_ctl_type);
            CIRRUS_LOG("    flags    = 0x%04X", c_flags);
            CIRRUS_LOG("    len      = %u", c_len);
            
            if (outImage->wmfwControlCount < 512) {
                WMFWControl &ctl = outImage->wmfwControls[outImage->wmfwControlCount++];
                ctl.offset = c_offset;
                ctl.type = c_type;
                ctl.size = c_size;
                strlcpy(ctl.name, c_name, sizeof(ctl.name));
                ctl.ctl_type = c_ctl_type;
                ctl.flags = c_flags;
                ctl.len = c_len;
                alg.controlCount++;
            }
            
            pos = coeff_end; // Skip to next coefficient safely
        }
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
        outImage->fw_core_rev = OSSwapLittleToHostInt32(header->rev);
        
        CIRRUS_LOG("WMFW File Header:");
        CIRRUS_LOG("  Magic    : 0x%08X", outImage->fw_magic);
        CIRRUS_LOG("  Version  : %u", outImage->fw_version);
        CIRRUS_LOG("  Core     : %u", outImage->fw_core);
        CIRRUS_LOG("  Core Rev : 0x%08X", outImage->fw_core_rev);
        
        size_t pos = OSSwapLittleToHostInt32(header->len);
        
        while (pos + sizeof(wmfw_region) <= size) {
            const wmfw_region *raw_region = (const wmfw_region *)&data[pos];
            uint32_t type_offset = OSSwapLittleToHostInt32(raw_region->type_offset_le);
            uint32_t type = (type_offset >> 24) & 0xFF;
            uint32_t offset = type_offset & 0xFFFFFF;
            uint32_t len = OSSwapLittleToHostInt32(raw_region->len);
            
            pos += sizeof(wmfw_region);
            
            if (pos + len > size) {
                CIRRUS_LOG("Error: Region payload exceeds file bounds");
                break;
            }
            
            if (outImage->regionCount < 32) {
                FirmwareRegion &reg = outImage->regions[outImage->regionCount++];
                reg.baseWordOffset = offset;
                reg.length = len;
                reg.data = raw_region->data;
                
                switch (type) {
                case WMFW_HALO_XM_PACKED: reg.regionType = RegionType::XM_PACKED; outImage->stat_xm_blocks++; break;
                case WMFW_HALO_YM_PACKED: reg.regionType = RegionType::YM_PACKED; outImage->stat_ym_blocks++; break;
                case WMFW_HALO_PM_PACKED: reg.regionType = RegionType::PM_PACKED; outImage->stat_pm_blocks++; break;
                case WMFW_ALGORITHM_DATA: 
                    reg.regionType = RegionType::ALGORITHM_DATA; 
                    CIRRUS_LOG("WMFW Block Type: ALGORITHM_DATA (0xF2)");
                    CIRRUS_LOG("WMFW Block Start Offset: 0x%08zX", pos);
                    CIRRUS_LOG("WMFW Block Payload Size: %u bytes", len);
                    parseWMFWAlgorithmData(raw_region->data, len, outImage, pos, outImage->fw_version);
                    break;
                case WMFW_METADATA:       reg.regionType = RegionType::METADATA; break;
                case WMFW_INFO_TEXT:      reg.regionType = RegionType::INFO_TEXT; break;
                case WMFW_NAME_TEXT:      reg.regionType = RegionType::NAME_TEXT; break;
                case WMFW_ABSOLUTE:       reg.regionType = RegionType::UNKNOWN; break;
                default:                  reg.regionType = RegionType::UNKNOWN; break;
                }
            }
            
            if (type == WMFW_HALO_XM_PACKED && offset == 0 && len >= kHaloIdHdrWords * 3) {
                // This is the first XM region containing the Halo ID Header.
                outImage->fw_id = readPacked24BE(raw_region->data, kHaloFwIdWord);
                outImage->n_algs = 1; 
                CIRRUS_LOG("parseWMFW: Found Halo Header. fw_id=0x%06X", outImage->fw_id);
            }
            
            pos += len;
        }
        
        return true;
    }

    static inline uint32_t readUnpacked32BE(const uint8_t *data, uint32_t wordIdx) {
        const uint32_t b = wordIdx * 4;
        return ((uint32_t)data[b] << 24) | ((uint32_t)data[b+1] << 16) | ((uint32_t)data[b+2] << 8) | (uint32_t)data[b+3];
    }

    static bool parseAlgorithmTable(const uint8_t *vmem, size_t vmem_size, FirmwareImage &outImage) {
        // We are reading from UNPACK24_0, where each word is a 32-bit big-endian value (4 bytes)
        const uint32_t kHaloAlgTableStartWord = 10;
        const uint32_t kHaloAlgEntryWords = 6;

        if (vmem_size < kHaloAlgTableStartWord * 4) {
            CIRRUS_ERR("Dump buffer too small for Halo ID header");
            return false;
        }

        uint32_t core_id = readUnpacked32BE(vmem, 0);
        uint32_t block_rev = readUnpacked32BE(vmem, 1);
        uint32_t vendor_id = readUnpacked32BE(vmem, 2);
        uint32_t fw_id = readUnpacked32BE(vmem, 3);
        uint32_t fw_ver = readUnpacked32BE(vmem, 4);
        uint32_t fw_xm_base = readUnpacked32BE(vmem, 5);
        uint32_t fw_xm_size = readUnpacked32BE(vmem, 6);
        uint32_t fw_ym_base = readUnpacked32BE(vmem, 7);
        uint32_t fw_ym_size = readUnpacked32BE(vmem, 8);
        
        CIRRUS_LOG("HALO Header");
        CIRRUS_LOG(" Core ID     = %u (0x%06X)", core_id, core_id);
        CIRRUS_LOG(" Block Rev   = %u (0x%06X)", block_rev, block_rev);
        CIRRUS_LOG(" Vendor      = %u (0x%06X)", vendor_id, vendor_id);
        CIRRUS_LOG(" Firmware ID = %u (0x%06X)", fw_id, fw_id);
        CIRRUS_LOG(" Version     = %u (0x%06X)", fw_ver, fw_ver);
        CIRRUS_LOG(" FW xm_base  = 0x%06X", fw_xm_base);

        // Treat Firmware ID as the first "Algorithm" Region since coefficients bind to it
        AlgorithmInfo &fwAlg = outImage.algorithms[outImage.algorithmCount++];
        fwAlg.id = fw_id;
        fwAlg.baseWordOffset = fw_xm_base;
        fwAlg.ymBaseWordOffset = fw_ym_base;
        fwAlg.size = fw_xm_size;
        fwAlg.region = RegionType::XM_PACKED;

        uint32_t n_algs = readUnpacked32BE(vmem, 9);
        CIRRUS_LOG("Algorithm count = %u", n_algs);

        if (n_algs > 100) {
            CIRRUS_LOG("Warning: n_algs=%u seems abnormally large, truncating to 100", n_algs);
            n_algs = 100;
        }

        if (vmem_size < (kHaloAlgTableStartWord + n_algs * kHaloAlgEntryWords) * 4) {
            CIRRUS_ERR("Dump buffer too small for %u algorithms", n_algs);
            return false;
        }

        uint32_t valid_algs = 0;
        
        for (uint32_t i = 0; i < n_algs && outImage.algorithmCount < 32; i++) {
            uint32_t base = kHaloAlgTableStartWord + i * kHaloAlgEntryWords;
            
            uint32_t alg_id = readUnpacked32BE(vmem, base);
            uint32_t alg_ver = readUnpacked32BE(vmem, base + 1);
            uint32_t alg_xm_base = readUnpacked32BE(vmem, base + 2);
            uint32_t alg_xm_size = readUnpacked32BE(vmem, base + 3); // Not technically size, but we'll read it
            uint32_t alg_ym_base = readUnpacked32BE(vmem, base + 4);
            uint32_t alg_ym_size = readUnpacked32BE(vmem, base + 5);
            
            CIRRUS_LOG("Algorithm[%u]", i);
            CIRRUS_LOG(" id = %u (0x%06X)", alg_id, alg_id);
            CIRRUS_LOG(" ver = 0x%06X", alg_ver);
            CIRRUS_LOG(" xm_base = 0x%06X", alg_xm_base);
            CIRRUS_LOG(" ym_base = 0x%06X", alg_ym_base);

            AlgorithmInfo &alg = outImage.algorithms[outImage.algorithmCount++];
            alg.id = alg_id;
            alg.baseWordOffset = alg_xm_base;
            alg.ymBaseWordOffset = alg_ym_base;
            alg.size = alg_xm_size;
            alg.region = RegionType::XM_PACKED; // Kext uses this for logic
            
            valid_algs++;
        }

        return valid_algs > 0;
    }

    static bool parseBIN(const uint8_t *data, size_t size, FirmwareImage *outImage) {
        if (!outImage) return false;
        
        // Header is wmfw_coeff_hdr: magic(4), len(4), rev(4), core(4)
        if (size < 12) {
            CIRRUS_ERR("BIN file too small for header");
            return false;
        }
        
        if (data[0] != 'W' || data[1] != 'M' || data[2] != 'D' || data[3] != 'R') {
            CIRRUS_ERR("BIN file invalid magic (expected WMDR)");
            return false;
        }
        
        uint32_t hdr_len = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24); // len is Little Endian
        if (hdr_len < 12 || hdr_len > size) {
            CIRRUS_ERR("BIN invalid header len %u", hdr_len);
            return false;
        }

        uint32_t pos = hdr_len; // Coefficients start immediately after the header
        
        while (pos < size && outImage->coefficientCount < 128) {
            if (size - pos < 20) { // wmfw_coeff_item is 20 bytes
                break;
            }
            
            CoefficientBlock &coeff = outImage->coefficients[outImage->coefficientCount++];
            
            coeff.offset = data[pos+0] | (data[pos+1] << 8); // offset is le16
            coeff.type = data[pos+2] | (data[pos+3] << 8); // type is le16
            coeff.id = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24); // id is le32
            // Skip ver(4) and sr(4)
            coeff.length = data[pos+16] | (data[pos+17] << 8) | (data[pos+18] << 16) | (data[pos+19] << 24); // len is le32
            
            pos += 20;
            
            if (pos + coeff.length > size) {
                CIRRUS_ERR("BIN coefficient block out of bounds: pos=%u len=%u size=%zu", pos, coeff.length, size);
                break;
            }
            
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
