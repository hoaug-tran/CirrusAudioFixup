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
static constexpr uint32_t kHaloFwIdWord          = 2;
static constexpr uint32_t kHaloNAlgsWord         = 8;
static constexpr uint32_t kHaloAlgTableStartWord = 9;
static constexpr uint32_t kHaloAlgEntryWords     = 6;
static constexpr uint32_t kHaloIdHdrWords        = 9;  // words before alg table

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
            
            // Do not extract immediately, wait until all regions are parsed
            
            pos += sizeof(wmfw_region) + len;
        }
        
        extractHaloAlgorithms(outImage);
        
        return true;
    }

    // Note: This is a Halo firmware specific extraction.
    // The Algorithm Table is scattered across multiple XM_PACKED regions.
    // We assemble a virtual memory buffer and parse it.
    //
    // wmfw_halo_id_hdr layout (each field = 1 DSP word = 3 bytes packed):
    //   word 0: core_id
    //   word 1: core_rev
    //   word 2: fw_id     <-- firmware ID
    //   word 3: fw_ver
    //   word 4: xm_base
    //   word 5: xm_size
    //   word 6: ym_base
    //   word 7: ym_size
    //   word 8: n_algs    <-- number of algorithms (0-indexed)
    // Then n_algs x wmfw_halo_alg_hdr (6 words each):
    //   word 0: alg_id
    //   word 1: alg_ver
    //   word 2: xm_base
    //   word 3: xm_size
    //   word 4: ym_base
    //   word 5: ym_size
    static void extractHaloAlgorithms(FirmwareImage *outImage) {
        size_t vmem_size = 0;

        for (uint32_t i = 0; i < outImage->regionCount; i++) {
            const FirmwareRegion &reg = outImage->regions[i];
            if (reg.regionType == RegionType::XM_PACKED) {
                size_t end_pos = (size_t)(reg.baseWordOffset * 3) + reg.length;
                if (end_pos > vmem_size) vmem_size = end_pos;
            }
        }

        if (vmem_size < kHaloIdHdrWords * 3) {  // need at least 9 words = 27 bytes
            CIRRUS_ERR("XM region too small (%zu bytes) for Halo ID header (need %u)", vmem_size, kHaloIdHdrWords * 3);
            return;
        }

        uint8_t *vmem = (uint8_t *)IOMalloc(vmem_size);
        if (!vmem) return;
        memset(vmem, 0, vmem_size);

        for (uint32_t i = 0; i < outImage->regionCount; i++) {
            const FirmwareRegion &reg = outImage->regions[i];
            if (reg.regionType == RegionType::XM_PACKED) {
                uint32_t byte_offset = reg.baseWordOffset * 3;
                if (byte_offset + reg.length <= vmem_size)
                    memcpy(vmem + byte_offset, reg.data, reg.length);
            }
        }

        // Use file-scope readPacked24BE() with named constants
        outImage->fw_id = readPacked24BE(vmem, kHaloFwIdWord);
        uint32_t n_algs = readPacked24BE(vmem, kHaloNAlgsWord);

        CIRRUS_LOG("Halo ID: fw_id=0x%06X xm_base=0x%06X xm_size=%u ym_base=0x%06X ym_size=%u n_algs=%u",
                   outImage->fw_id,
                   readPacked24BE(vmem, 4), readPacked24BE(vmem, 5),
                   readPacked24BE(vmem, 6), readPacked24BE(vmem, 7),
                   n_algs);

        if (n_algs == 0 || n_algs > 32) {
            CIRRUS_ERR("n_algs=%u out of range [1-32]; aborting algorithm extraction", n_algs);
            IOFree(vmem, vmem_size);
            return;
        }

        // Bounds check: make sure the algorithm table fits within vmem
        size_t requiredBytes = (kHaloAlgTableStartWord + n_algs * kHaloAlgEntryWords) * 3;
        if (requiredBytes > vmem_size) {
            CIRRUS_ERR("Algorithm table OOB: n_algs=%u required=%zu vmem=%zu", n_algs, requiredBytes, vmem_size);
            IOFree(vmem, vmem_size);
            return;
        }

        // Parse algorithm entries: start at word kHaloAlgTableStartWord, each entry = kHaloAlgEntryWords words
        for (uint32_t i = 0; i < n_algs && outImage->algorithmCount < 32; i++) {
            uint32_t base   = kHaloAlgTableStartWord + i * kHaloAlgEntryWords;
            uint32_t alg_id = readPacked24BE(vmem, base);
            if (alg_id == 0 || alg_id == 0xFFFFFF) continue;

            AlgorithmInfo &alg = outImage->algorithms[outImage->algorithmCount++];
            alg.id             = alg_id;
            alg.baseWordOffset = readPacked24BE(vmem, base + 2); // xm_base
            alg.size           = readPacked24BE(vmem, base + 3); // xm_size
            alg.region         = RegionType::XM_PACKED;

            CIRRUS_LOG("Algorithm[%u]: id=0x%06X ver=0x%06X XM@0x%06X+%u YM@0x%06X+%u",
                       i, alg_id,
                       readPacked24BE(vmem, base+1),
                       readPacked24BE(vmem, base+2), readPacked24BE(vmem, base+3),
                       readPacked24BE(vmem, base+4), readPacked24BE(vmem, base+5));
        }

        CIRRUS_LOG("extractHaloAlgorithms: found %u algorithms", outImage->algorithmCount);
        IOFree(vmem, vmem_size);
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
