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
    uint32_t n_algs; // Expected number of algorithms from header
    uint32_t xm_dump_crc; // CRC32 of the live XM dump
    
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
    MappedRegion regions[MAX_MAPPED_REGIONS]; // 32 firmware + up to 128 coefficient blocks
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
                        alg_xm_base = image.algorithms[a].baseWordOffset;
                        // For Halo, ym_base is typically after xm_size in the struct, but we didn't save ym_base in AlgorithmInfo!
                        // Let's assume algorithm type is XM for now or we must fetch YM.
                        // Wait, Linux maps based on coeff.type.
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
            // WMFW_HALO_XM_PACKED = 0x11, YM_PACKED = 0x12
            if (type_masked == WMFW_HALO_XM_PACKED) {
                outReg.regionType = RegionType::XM_PACKED;
                outReg.firmwareAddress = alg_xm_base + coeff.offset;
                MappingStatus status = mapPackedAddress(outReg.regionType, outReg.firmwareAddress, 0, outReg.dspRegister);
                if (status != MappingStatus::OK) return false;
            } else if (type_masked == WMFW_HALO_YM_PACKED) {
                // We need ym_base. For now we skip or we add ym_base to AlgorithmInfo.
                CIRRUS_LOG("YM_PACKED Coeff mapping not yet implemented");
                continue;
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
        
        size_t pos = OSSwapLittleToHostInt32(header->len);
        
        while (pos < size && outImage->regionCount < 32) {
            const wmfw_region *raw_region = (const wmfw_region *)&data[pos];
            
            uint32_t type_offset = OSSwapLittleToHostInt32(raw_region->type_offset_le);
            uint32_t type = (type_offset >> 24) & 0xFF;
            uint32_t offset = type_offset & 0xFFFFFF;
            uint32_t len = OSSwapLittleToHostInt32(raw_region->len);
            
            if (pos + sizeof(wmfw_region) + len > size) {
                CIRRUS_ERR("WMFW region length out of bounds: offset=%zu len=%u size=%zu", pos, len, size);
                break;
            }
            
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
            
            if (type == WMFW_HALO_XM_PACKED && offset == 0 && len >= kHaloIdHdrWords * 3) {
                // This is the first XM region containing the Halo ID Header.
                outImage->fw_id = readPacked24BE(raw_region->data, kHaloFwIdWord);
                // In our updated ground truth, the first Algorithm ID is at word 9.
                // We don't have a strict n_algs field. We'll set it to 1 for the WMFW summary.
                outImage->n_algs = 1; 
                CIRRUS_LOG("parseWMFW: Found Halo Header. fw_id=0x%06X", outImage->fw_id);
            }
            
            pos += sizeof(wmfw_region) + len;
        }
        
        return true;
    }

    static bool parseAlgorithmTable(const uint8_t *vmem, size_t vmem_size, FirmwareImage &outImage) {
        if (vmem_size < kHaloIdHdrWords * 3) {
            CIRRUS_ERR("Dump buffer too small (%zu bytes) for Halo ID header", vmem_size);
            return false;
        }

        // Scanner for candidates in the entire dump
        uint32_t header_offset = 0xFFFFFFFF;
        for (uint32_t off = 0; off + 40 <= vmem_size; off += 3) {
            uint32_t scan_fw_id = readPacked24BE(vmem, off / 3 + kHaloFwIdWord);
            
            if (scan_fw_id == outImage.fw_id || (scan_fw_id > 0 && scan_fw_id < 0xFFFFFF)) {
                uint32_t scan_core = readPacked24BE(vmem, off / 3);
                if (true) {
                    uint32_t crc40 = CirrusFirmwareParser::calculate_crc32(vmem + off, 40);
                    CIRRUS_LOG("==== Candidate Header @ 0x%04X ====", off);
                    CIRRUS_LOG("Decoded:");
                    CIRRUS_LOG("core_id = 0x%06X", readPacked24BE(vmem, off/3 + 0));
                    CIRRUS_LOG("core_rev= 0x%06X", readPacked24BE(vmem, off/3 + 1));
                    CIRRUS_LOG("fw_id   = 0x%06X", readPacked24BE(vmem, off/3 + 2));
                    CIRRUS_LOG("version = 0x%06X", readPacked24BE(vmem, off/3 + 3));
                    CIRRUS_LOG("vndr_id = 0x%06X", readPacked24BE(vmem, off/3 + 4));
                    CIRRUS_LOG("xm_base = 0x%06X", readPacked24BE(vmem, off/3 + 5));
                    CIRRUS_LOG("xm_size = 0x%06X", readPacked24BE(vmem, off/3 + 6));
                    CIRRUS_LOG("ym_base = 0x%06X", readPacked24BE(vmem, off/3 + 7));
                    CIRRUS_LOG("ym_size = 0x%06X", readPacked24BE(vmem, off/3 + 8));
                    CIRRUS_LOG("Alg0 ID = 0x%06X", readPacked24BE(vmem, off/3 + 9));
                    CIRRUS_LOG("Alg0 Ver= 0x%06X", readPacked24BE(vmem, off/3 + 10));
                    CIRRUS_LOG("CRC(40) = 0x%08X", crc40);
                    
                    if (header_offset == 0xFFFFFFFF) {
                        if (scan_fw_id == outImage.fw_id) {
                            header_offset = off;
                        } else {
                            header_offset = off;
                        }
                    }
                }
            }
        }
        
        if (header_offset == 0xFFFFFFFF) {
            CIRRUS_ERR("parseAlgorithmTable: No valid Halo Header candidate found. Aborting parser.");
            return false;
        }
        
        uint32_t hdr_base_word = header_offset / 3;
        CIRRUS_LOG("parseAlgorithmTable: Selected Header @ 0x%04X", header_offset);
        
        // We scan algorithms until we hit a known terminator or run out of space.
        // We will just scan up to 10 entries max.
        uint32_t n_algs_scan = 10;
        
        size_t requiredBytes = header_offset + (kHaloAlgTableStartWord + kHaloAlgEntryWords) * 3;
        if (vmem_size < requiredBytes) {
            CIRRUS_ERR("Dump buffer too small to hold even one Algorithm Entry");
            return false;
        }

        uint32_t valid_algs = 0;
        uint32_t discarded_algs = 0;
        uint32_t invalid_run = 0;
        
        for (uint32_t i = 0; i < n_algs_scan && outImage.algorithmCount < 32; i++) {
            uint32_t base   = hdr_base_word + kHaloAlgTableStartWord + i * kHaloAlgEntryWords;
            if (base * 3 + kHaloAlgEntryWords * 3 > vmem_size) {
                CIRRUS_LOG("parseAlgorithmTable: Reached end of memory buffer.");
                break;
            }
            
            uint32_t alg_id = readPacked24BE(vmem, base);
            uint32_t alg_ver = readPacked24BE(vmem, base+1);
            uint32_t alg_xm_base = readPacked24BE(vmem, base+2);
            
            // Validation: discard dummy or malformed entries
            if (alg_id == 0 || alg_id == 0xFFFFFF) {
                CIRRUS_LOG("Discard: index=%u id=0x%06X xm=0x%06X reason=Invalid ID", i, alg_id, alg_xm_base);
                discarded_algs++;
                invalid_run++;
                if (invalid_run >= 5) break;
                continue;
            }
            
            // Heuristic check for uninitialized or dummy pointer values
            if (alg_xm_base == 0x00BEDE || alg_xm_base == 0xBEDEAD || alg_xm_base == 0xFFFFFF) {
                CIRRUS_LOG("Discard: index=%u id=0x%06X xm=0x%06X reason=Invalid XM Base", i, alg_id, alg_xm_base);
                discarded_algs++;
                invalid_run++;
                if (invalid_run >= 5) break;
                continue;
            }
            
            invalid_run = 0; // reset run on valid entry

            AlgorithmInfo &alg = outImage.algorithms[outImage.algorithmCount++];
            alg.id             = alg_id;
            alg.baseWordOffset = readPacked24BE(vmem, base + 2); // xm_base
            alg.size           = 0; // Not available in 5-word struct
            alg.region         = RegionType::XM_PACKED;

            CIRRUS_LOG("Algorithm[%u]: id=%u (0x%06X) ver=0x%06X xm_base=0x%06X ym_base=0x%06X pm_base=0x%06X",
                       i, alg_id, alg_id, alg_ver,
                       readPacked24BE(vmem, base+2),
                       readPacked24BE(vmem, base+3),
                       readPacked24BE(vmem, base+4));
            valid_algs++;
        }

        CIRRUS_LOG("==== Algorithm Parser ====");
        CIRRUS_LOG("Found            %u entries", valid_algs + discarded_algs);
        CIRRUS_LOG("Valid            %u", valid_algs);
        CIRRUS_LOG("Discarded        %u", discarded_algs);
        return true;
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
