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

// Intermediate Representation
struct FirmwareSpan {
    const uint8_t *begin;
    uint32_t size;
};

struct FirmwareRegion {
    uint32_t type;
    uint32_t offset;
    FirmwareSpan data;
    uint32_t crc;
};

struct FirmwareAlgorithm {
    uint32_t id;
    uint32_t ver;
};

#define MAX_FIRMWARE_REGIONS 128
#define MAX_FIRMWARE_ALGORITHMS 16

struct FirmwareImage {
    uint32_t fw_magic;
    uint32_t fw_version;
    uint32_t fw_total_bytes;
    uint32_t fw_crc;
    uint8_t fw_core;
    uint16_t fw_core_rev;
    
    FirmwareRegion regions[MAX_FIRMWARE_REGIONS];
    uint32_t regionCount;
    
    FirmwareAlgorithm algorithms[MAX_FIRMWARE_ALGORITHMS];
    uint32_t algorithmCount;
    
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
        
        while (pos < size && outImage->regionCount < MAX_FIRMWARE_REGIONS) {
            const wmfw_region *raw_region = (const wmfw_region *)&data[pos];
            
            uint32_t type = OSSwapBigToHostInt32(raw_region->type_be) & 0xFF;
            uint32_t offset = OSSwapLittleToHostInt32(raw_region->offset_le) & 0xFFFFFF;
            uint32_t len = OSSwapLittleToHostInt32(raw_region->len);
            
            FirmwareRegion &reg = outImage->regions[outImage->regionCount++];
            reg.type = type;
            reg.offset = offset;
            reg.data.begin = raw_region->data;
            reg.data.size = len;
            reg.crc = calculate_crc32(raw_region->data, len);
            
            switch (type) {
                case WMFW_HALO_XM_PACKED: outImage->stat_xm_blocks++; break;
                case WMFW_HALO_YM_PACKED: outImage->stat_ym_blocks++; break;
                case WMFW_HALO_PM_PACKED: outImage->stat_pm_blocks++; break;
                case WMFW_ALGORITHM_DATA: outImage->stat_coeff_blocks++; break;
                case WMFW_METADATA:
                case WMFW_INFO_TEXT:
                case WMFW_NAME_TEXT: outImage->stat_metadata_blocks++; break;
                case WMFW_ABSOLUTE: outImage->stat_metadata_blocks++; break; // Typically configuration info
                default:
                    CIRRUS_LOG("WMFW Notice: Unknown block type 0x%02X at offset %zu", type, pos);
                    outImage->stat_unknown_blocks++;
                    break;
            }
            
            pos += sizeof(wmfw_region) + len;
        }
        
        outImage->fingerprint = calculate_crc32((const uint8_t*)&outImage->fw_crc, 4); // Basic fingerprint for now, can be augmented with BIN CRC and SSID later
        
        return true;
    }
};

#endif
