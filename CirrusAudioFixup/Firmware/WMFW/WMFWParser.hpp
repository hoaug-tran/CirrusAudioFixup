#pragma once

#include <IOKit/IOLib.h>

#define WMFW_MAGIC_0 'W'
#define WMFW_MAGIC_1 'M'
#define WMFW_MAGIC_2 'F'
#define WMFW_MAGIC_3 'W'

#define WMFW_ABSOLUTE 0xf0
#define WMFW_ALGORITHM_DATA 0xf2
#define WMFW_METADATA 0xfc
#define WMFW_NAME_TEXT 0xfe
#define WMFW_INFO_TEXT 0xff

#define WMFW_HALO_PM_PACKED 0x10
#define WMFW_HALO_XM_PACKED 0x11
#define WMFW_HALO_YM_PACKED 0x12
#define WMFW_ADSP2_XM 0x05
#define WMFW_ADSP2_YM 0x06

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
#define MAX_MAPPED_REGIONS 192

enum class RegionType : uint32_t {
    PmPacked = 0x10,
    XmPacked = 0x11,
    YmPacked = 0x12,
    XmUnpacked = 0x5,
    YmUnpacked = 0x6,
    AlgorithmData = 0xF2,
    Metadata = 0xFC,
    NameText = 0xFE,
    InfoText = 0xFF,
    Unknown = 0xFFFF,

    PM_PACKED = PmPacked,
    XM_PACKED = XmPacked,
    YM_PACKED = YmPacked,
    XM_UNPACKED = XmUnpacked,
    YM_UNPACKED = YmUnpacked,
    ALGORITHM_DATA = AlgorithmData,
    METADATA = Metadata,
    NAME_TEXT = NameText,
    INFO_TEXT = InfoText,
    UNKNOWN = Unknown,
};

enum class MappingStatus {
    Ok = 0,
    UnsupportedRegion,
    Overflow,
    AlignmentError,
    InvalidOffset,
    OK = Ok,
};

struct FirmwareSpan {
    const uint8_t* begin;
    uint32_t size;
};

struct FirmwareRegion {
    RegionType regionType;
    uint32_t baseWordOffset;
    const uint8_t* data;
    uint32_t length;
};

struct AlgorithmInfo {
    uint32_t id;
    uint32_t version;
    RegionType region;
    uint32_t baseWordOffset;
    uint32_t ymBaseWordOffset;
    uint32_t size;
};

struct CoefficientBlock {
    uint32_t id;
    uint32_t version;
    uint16_t type;
    uint32_t offset;
    uint32_t length;
    uint32_t payloadCrc;
    const uint8_t* data;
};

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
    const WMFWAlgorithm* algorithm;
    const WMFWControl* control;
};

struct FirmwareImage {
    union {
        uint32_t magic;
        uint32_t fw_magic;
    };
    union {
        uint32_t version;
        uint32_t fw_version;
    };
    union {
        uint32_t totalBytes;
        uint32_t fw_total_bytes;
    };
    union {
        uint32_t crc;
        uint32_t fw_crc;
    };
    union {
        uint32_t core;
        uint32_t fw_core;
    };
    union {
        uint32_t coreRevision;
        uint32_t fw_core_rev;
    };

    union {
        uint32_t firmwareId;
        uint32_t fw_id;
    };
    union {
        uint32_t haloFirmwareVersion;
        uint32_t halo_fw_version;
    };
    union {
        uint32_t algorithmTotal;
        uint32_t n_algs;
    };
    union {
        uint32_t xmDumpCrc;
        uint32_t xm_dump_crc;
    };

    uint32_t algorithmCount;
    AlgorithmInfo algorithms[32];

    union {
        uint32_t algorithmId;
        uint32_t algorithm_id;
    };
    union {
        uint32_t algorithmVersion;
        uint32_t algorithm_version;
    };
    union {
        uint32_t algorithmXmBase;
        uint32_t algorithm_xm_base;
    };
    union {
        uint32_t algorithmXmSize;
        uint32_t algorithm_xm_size;
    };
    union {
        uint32_t algorithmYmBase;
        uint32_t algorithm_ym_base;
    };
    union {
        uint32_t algorithmYmSize;
        uint32_t algorithm_ym_size;
    };

    uint32_t wmfwAlgorithmCount;
    WMFWAlgorithm wmfwAlgorithms[8];

    uint32_t wmfwControlCount;
    WMFWControl wmfwControls[512];

    uint32_t regionCount;
    FirmwareRegion regions[32];

    uint32_t coefficientCount;
    CoefficientBlock coefficients[128];
    union {
        uint32_t totalCoeffPayloadBytes;
        uint32_t total_coeff_payload_bytes;
    };

    union {
        uint32_t statXmBlocks;
        uint32_t stat_xm_blocks;
    };
    union {
        uint32_t statYmBlocks;
        uint32_t stat_ym_blocks;
    };
    union {
        uint32_t statPmBlocks;
        uint32_t stat_pm_blocks;
    };
    union {
        uint32_t statCoeffBlocks;
        uint32_t stat_coeff_blocks;
    };
    union {
        uint32_t statMetadataBlocks;
        uint32_t stat_metadata_blocks;
    };
    union {
        uint32_t statUnknownBlocks;
        uint32_t stat_unknown_blocks;
    };

    uint32_t fingerprint;
};

struct MappedRegion {
    RegionType regionType;
    uint32_t firmwareAddress;
    uint32_t dspRegister;
    uint32_t size;
    FirmwareSpan data;
};

struct MappedImage {
    MappedRegion regions[MAX_MAPPED_REGIONS];
    uint32_t regionCount;
    uint32_t mappingCrc;
};

struct HaloMemoryPointer {
    uint8_t region;
    uint16_t wordOffset;
};

inline HaloMemoryPointer decodePointer(uint32_t value) {
    return {static_cast<uint8_t>(value >> 16), static_cast<uint16_t>(value & 0xFFFF)};
}

class CirrusFirmwareMapper {
public:
    static MappingStatus mapPackedAddress(RegionType type, uint32_t wordOffset, uint32_t byteOffset, uint32_t& regAddress) {
        uint64_t base = 0, stride = 0;
        switch (type) {
        case RegionType::PM_PACKED:
            base = 0x03800000;
            stride = 5;
            break;
        case RegionType::XM_PACKED:
            base = 0x02000000;
            stride = 3;
            break;
        case RegionType::YM_PACKED:
            base = 0x02C00000;
            stride = 3;
            break;
        case RegionType::XM_UNPACKED:
            base = 0x02800000;
            stride = 4;
            break;
        case RegionType::YM_UNPACKED:
            base = 0x03400000;
            stride = 4;
            break;
        default:
            return MappingStatus::UnsupportedRegion;
        }
        uint64_t address = base + uint64_t(wordOffset) * stride;
        if (stride == 3)
            address &= ~uint64_t(3);
        address += byteOffset;
        if (address > 0xFFFFFFFFULL)
            return MappingStatus::Overflow;
        regAddress = static_cast<uint32_t>(address);
        return MappingStatus::OK;
    }

    static bool mapFirmwareImage(const FirmwareImage& image, MappedImage& outMapped) {
        outMapped.regionCount = 0;
        outMapped.mappingCrc = 0xFFFFFFFF;
        if (image.regionCount > MAX_FIRMWARE_REGIONS)
            return false;

        for (uint32_t i = 0; i < image.regionCount; i++) {
            if (outMapped.regionCount >= MAX_MAPPED_REGIONS) {
                CIRRUS_ERR("mapFirmwareImage: MAX_MAPPED_REGIONS overflow at region %d", i);
                return false;
            }
            const FirmwareRegion& inReg = image.regions[i];
            MappedRegion& outReg = outMapped.regions[outMapped.regionCount];

            outReg.regionType = inReg.regionType;

            outReg.firmwareAddress = inReg.baseWordOffset;
            outReg.size = inReg.length;
            outReg.data.begin = inReg.data;
            outReg.data.size = inReg.length;
            outReg.dspRegister = 0;

            if (outReg.regionType == RegionType::PM_PACKED || outReg.regionType == RegionType::XM_PACKED ||
                outReg.regionType == RegionType::YM_PACKED || outReg.regionType == RegionType::XM_UNPACKED ||
                outReg.regionType == RegionType::YM_UNPACKED) {
                MappingStatus status = mapPackedAddress(outReg.regionType, outReg.firmwareAddress, 0, outReg.dspRegister);
                if (status != MappingStatus::OK) {
                    CIRRUS_ERR("Mapping failed for region %d: status %d", i, (int)status);
                    return false;
                }
            } else {
                outReg.dspRegister = 0xFFFFFFFF;
            }

            const uint8_t* crcData = (const uint8_t*)&outReg;

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

    static bool mapCoefficients(const FirmwareImage& image, MappedImage& outMapped) {
        outMapped.regionCount = 0;
        outMapped.mappingCrc = 0xFFFFFFFF;
        if (image.coefficientCount > 128 || image.algorithmCount > 32)
            return false;

        for (uint32_t i = 0; i < image.coefficientCount; i++) {
            if (outMapped.regionCount >= MAX_MAPPED_REGIONS) {
                CIRRUS_ERR("mapCoefficients: MAX_MAPPED_REGIONS overflow");
                return false;
            }
            const CoefficientBlock& coeff = image.coefficients[i];
            MappedRegion& outReg = outMapped.regions[outMapped.regionCount];
            uint32_t type_masked = coeff.type;
            uint32_t byteOffset = coeff.offset;
            uint32_t algorithmBase = 0;
            bool found = false;

            if (type_masked == WMFW_HALO_XM_PACKED || type_masked == 0x5) {
                outReg.regionType = (type_masked == 0x5) ? RegionType::XM_UNPACKED : RegionType::XM_PACKED;
                for (uint32_t a = 0; a < image.algorithmCount; a++) {
                    if (image.algorithms[a].id == coeff.id) {
                        algorithmBase = image.algorithms[a].baseWordOffset;
                        found = true;
                        break;
                    }
                }
            } else if (type_masked == WMFW_HALO_YM_PACKED || type_masked == 0x6) {
                outReg.regionType = (type_masked == 0x6) ? RegionType::YM_UNPACKED : RegionType::YM_PACKED;
                for (uint32_t a = 0; a < image.algorithmCount; a++) {
                    if (image.algorithms[a].id == coeff.id) {
                        algorithmBase = image.algorithms[a].ymBaseWordOffset;
                        found = true;
                        break;
                    }
                }
            } else if (coeff.type == (WMFW_NAME_TEXT << 8) || coeff.type == (WMFW_INFO_TEXT << 8) || coeff.type == (WMFW_METADATA << 8)) {
                CIRRUS_LOG("Skipping non-memory coefficient block %u (ID=0x%06X type=0x%X)", i, coeff.id, type_masked);
                continue;
            } else {
                CIRRUS_ERR("Unsupported coefficient type 0x%X", coeff.type);
                return false;
            }

            if (!found) {
                CIRRUS_ERR("No algorithm 0x%06X for coefficient block %u type=0x%X", coeff.id, i, type_masked);
                return false;
            }

            outReg.firmwareAddress = algorithmBase;
            MappingStatus status = mapPackedAddress(outReg.regionType, algorithmBase, byteOffset, outReg.dspRegister);
            if (status != MappingStatus::OK)
                return false;
            CIRRUS_LOG("Coefficient %u: ID=0x%06X type=0x%X alg_base=0x%06X byte_offset=0x%08X mapped=0x%08X", i, coeff.id, type_masked,
                       algorithmBase, byteOffset, outReg.dspRegister);

            outReg.size = coeff.length;
            outReg.data.begin = coeff.data;
            outReg.data.size = coeff.length;

            const uint8_t* crcData = (const uint8_t*)&outReg;
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
    static uint32_t calculateCrc32(const uint8_t* data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++) {
            crc ^= data[i];
            for (size_t j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return ~crc;
    }

    static inline uint32_t calculate_crc32(const uint8_t* data, size_t length) { return calculateCrc32(data, length); }

    static bool validateWMFW(const uint8_t* data, size_t size) {
        if (!data || size < 40 || size > 0xFFFFFFFFULL || memcmp(data, "WMFW", 4))
            return false;

        if (readLE32(data + 4) != 40 || data[10] != 4 || data[11] != 3)
            return false;
        size_t pos = 40;
        uint32_t count = 0;
        while (pos < size) {
            if (size - pos < 8 || ++count > MAX_FIRMWARE_REGIONS)
                return false;
            uint32_t len = readLE32(data + pos + 4);
            pos += 8;
            if (len > size - pos)
                return false;
            pos += len;
        }
        return count != 0;
    }

    static inline uint32_t readLE32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    static inline uint16_t readLE16(const uint8_t* p) { return p[0] | (p[1] << 8); }

    static inline uint32_t alignStringLen(uint32_t strLen, uint32_t fieldBytes) { return ((strLen + fieldBytes) + 3) & ~0x03; }

    static uint32_t regionToReg(uint16_t type, uint32_t dspWord) {
        uint32_t reg = 0;
        return CirrusFirmwareMapper::mapPackedAddress(static_cast<RegionType>(type), dspWord, 0, reg) == MappingStatus::OK ? reg : 0;
    }

    static bool resolveControl(const FirmwareImage& fw, const WMFWControlRef& ref, uint32_t& reg) {
        reg = 0;
        if (!ref.algorithm || !ref.control || fw.algorithmCount > 32)
            return false;
        const auto& ctl = *ref.control;
        if (!ctl.len)
            return false;
        for (uint32_t i = 0; i < fw.algorithmCount; ++i) {
            const auto& alg = fw.algorithms[i];
            if (alg.id != ref.algorithm->id)
                continue;
            uint32_t base;
            switch (ctl.type) {
            case WMFW_ADSP2_XM:
            case WMFW_HALO_XM_PACKED:
                base = alg.baseWordOffset;
                break;
            case WMFW_ADSP2_YM:
            case WMFW_HALO_YM_PACKED:
                base = alg.ymBaseWordOffset;
                break;
            default:
                return false;
            }
            if (base > 0xFFFFFFFFu - ctl.offset)
                return false;
            return CirrusFirmwareMapper::mapPackedAddress(static_cast<RegionType>(ctl.type), base + ctl.offset, 0, reg) ==
                   MappingStatus::OK;
        }
        return false;
    }

    static bool findControl(const FirmwareImage* fw, const char* name, WMFWControlRef& out) {
        out = {};
        if (!fw || !name || fw->wmfwAlgorithmCount > 8 || fw->wmfwControlCount > 512)
            return false;
        for (uint32_t i = 0; i < fw->wmfwAlgorithmCount; i++) {
            const WMFWAlgorithm& alg = fw->wmfwAlgorithms[i];
            if (alg.firstControl > fw->wmfwControlCount || alg.controlCount > fw->wmfwControlCount - alg.firstControl)
                return false;
            for (uint32_t j = 0; j < alg.controlCount; j++) {
                const WMFWControl& ctl = fw->wmfwControls[alg.firstControl + j];
                if (strncmp(ctl.name, name, sizeof(ctl.name)) == 0) {
                    out.algorithm = &alg;
                    out.control = &ctl;
                    return true;
                }
            }
        }
        return false;
    }

    static bool parseWMFWAlgorithmData(const uint8_t* data, size_t size, FirmwareImage* outImage, size_t file_offset, uint8_t fw_version) {
        (void)file_offset;
        if (size < 4)
            return false;

        if (fw_version < 2) {
            CIRRUS_LOG("Warning: This parser is designed for wmfw_ver >= 2. Skipping.");
            return false;
        }

        uint32_t pos = 0;

        if (pos + 4 > size)
            return false;
        uint32_t alg_id = readLE32(&data[pos]);
        pos += 4;

        if (pos + 1 > size)
            return false;
        uint8_t alg_name_len = data[pos];
        if (pos + alignStringLen(alg_name_len, 1) > size)
            return false;
        char alg_name[256] = {0};
        memcpy(alg_name, &data[pos + 1], alg_name_len);
        pos += alignStringLen(alg_name_len, 1);

        if (pos + 2 > size)
            return false;
        uint16_t alg_desc_len = readLE16(&data[pos]);
        if (pos + alignStringLen(alg_desc_len, 2) > size)
            return false;
        pos += alignStringLen(alg_desc_len, 2);

        if (pos + 4 > size)
            return false;
        uint32_t ncoeff = readLE32(&data[pos]);
        pos += 4;

        CIRRUS_LOG("Algorithm:");
        CIRRUS_LOG("  id      = %u (0x%08X)", alg_id, alg_id);
        CIRRUS_LOG("  name    = %s", alg_name);
        CIRRUS_LOG("  coeffs  = %u", ncoeff);

        if (outImage->wmfwAlgorithmCount >= 8) {
            CIRRUS_LOG("Warning: Maximum algorithms reached.");
            return false;
        }

        WMFWAlgorithm& alg = outImage->wmfwAlgorithms[outImage->wmfwAlgorithmCount++];
        alg.id = alg_id;
        strlcpy(alg.name, alg_name, sizeof(alg.name));
        alg.firstControl = outImage->wmfwControlCount;
        alg.controlCount = 0;

        for (uint32_t i = 0; i < ncoeff; i++) {
            if (pos + 8 > size) {
                CIRRUS_LOG("Sanity Check Failed: pos (0x%04X) exceeds payload size", pos);
                return false;
            }

            uint16_t c_offset = readLE16(&data[pos]);
            uint16_t c_type = readLE16(&data[pos + 2]);
            uint32_t c_size = readLE32(&data[pos + 4]);

            uint32_t payload_start = pos + 8;
            if (c_size > size - payload_start)
                return false;
            uint32_t coeff_end = payload_start + c_size;

            if (c_size < 8) {
                CIRRUS_LOG("Sanity Check Failed: Coeff[%u] c_size (%u) is suspiciously small", i, c_size);
                return false;
            }
            if (coeff_end > size) {
                CIRRUS_LOG("Sanity Check Failed: Coeff[%u] c_size (%u) exceeds payload", i, c_size);
                return false;
            }

            uint32_t inner_pos = payload_start;

            if (inner_pos + 1 > coeff_end)
                return false;
            uint8_t c_name_len = data[inner_pos];
            if (inner_pos + alignStringLen(c_name_len, 1) > coeff_end)
                return false;
            char c_name[256] = {0};
            memcpy(c_name, &data[inner_pos + 1], c_name_len);
            inner_pos += alignStringLen(c_name_len, 1);

            if (inner_pos + 1 > coeff_end)
                return false;
            uint8_t c_desc_len = data[inner_pos];
            if (inner_pos + alignStringLen(c_desc_len, 1) > coeff_end)
                return false;
            inner_pos += alignStringLen(c_desc_len, 1);

            if (inner_pos + 2 > coeff_end)
                return false;
            uint16_t c_unknown_len = readLE16(&data[inner_pos]);
            if (inner_pos + alignStringLen(c_unknown_len, 2) > coeff_end)
                return false;
            inner_pos += alignStringLen(c_unknown_len, 2);

            if (inner_pos + 8 > coeff_end)
                return false;
            uint16_t c_ctl_type = readLE16(&data[inner_pos]);
            uint16_t c_flags = readLE16(&data[inner_pos + 2]);
            uint32_t c_len = readLE32(&data[inner_pos + 4]);

            CIRRUS_LOG("  Coeff[%u] %s", i, c_name);
            CIRRUS_LOG("    offset   = 0x%04X", c_offset);
            CIRRUS_LOG("    type     = 0x%04X", c_type);
            CIRRUS_LOG("    ctl_type = 0x%04X", c_ctl_type);
            CIRRUS_LOG("    flags    = 0x%04X", c_flags);
            CIRRUS_LOG("    len      = %u", c_len);

            if (outImage->wmfwControlCount >= 512)
                return false;
            {
                WMFWControl& ctl = outImage->wmfwControls[outImage->wmfwControlCount++];
                ctl.offset = c_offset;
                ctl.type = c_type;
                ctl.size = c_size;
                strlcpy(ctl.name, c_name, sizeof(ctl.name));
                ctl.ctl_type = c_ctl_type;
                ctl.flags = c_flags;
                ctl.len = c_len;
                alg.controlCount++;
            }

            pos = coeff_end;
        }
        return pos == size;
    }

    static bool parseWMFW(const uint8_t* data, size_t size, FirmwareImage* outImage) {
        if (!outImage)
            return false;
        if (parseWMFWBody(data, size, outImage))
            return true;
        memset(outImage, 0, sizeof(*outImage));
        return false;
    }

    static bool parseWMFWBody(const uint8_t* data, size_t size, FirmwareImage* outImage) {
        if (!outImage)
            return false;
        memset(outImage, 0, sizeof(FirmwareImage));

        if (!validateWMFW(data, size)) {
            return false;
        }

        const wmfw_header* header = (const wmfw_header*)data;
        outImage->magic = (header->magic[0] << 24) | (header->magic[1] << 16) | (header->magic[2] << 8) | header->magic[3];
        outImage->version = header->ver;
        outImage->totalBytes = (uint32_t)size;
        outImage->crc = calculate_crc32(data, size);
        outImage->core = header->core;
        outImage->coreRevision = readLE16(data + 8);

        CIRRUS_LOG("WMFW File Header:");
        CIRRUS_LOG("  Magic    : 0x%08X", outImage->magic);
        CIRRUS_LOG("  Version  : %u", outImage->version);
        CIRRUS_LOG("  Core     : %u", outImage->core);
        CIRRUS_LOG("  Core Rev : 0x%08X", outImage->coreRevision);

        size_t pos = OSSwapLittleToHostInt32(header->len);

        while (pos + sizeof(wmfw_region) <= size) {
            const wmfw_region* raw_region = (const wmfw_region*)&data[pos];
            uint32_t type_offset = OSSwapLittleToHostInt32(raw_region->type_offset_le);
            uint32_t type = (type_offset >> 24) & 0xFF;
            uint32_t offset = type_offset & 0xFFFFFF;
            uint32_t len = OSSwapLittleToHostInt32(raw_region->len);

            pos += sizeof(wmfw_region);

            if (pos + len > size) {
                CIRRUS_LOG("Error: Region payload exceeds file bounds");
                return false;
            }

            if (outImage->regionCount < 32) {
                FirmwareRegion& reg = outImage->regions[outImage->regionCount++];
                reg.baseWordOffset = offset;
                reg.length = len;
                reg.data = raw_region->data;

                switch (type) {
                case WMFW_HALO_XM_PACKED:
                    reg.regionType = RegionType::XM_PACKED;
                    outImage->statXmBlocks++;
                    break;
                case WMFW_HALO_YM_PACKED:
                    reg.regionType = RegionType::YM_PACKED;
                    outImage->statYmBlocks++;
                    break;
                case WMFW_HALO_PM_PACKED:
                    reg.regionType = RegionType::PM_PACKED;
                    outImage->statPmBlocks++;
                    break;
                case WMFW_ALGORITHM_DATA:
                    reg.regionType = RegionType::ALGORITHM_DATA;
                    CIRRUS_LOG("WMFW Block Type: ALGORITHM_DATA (0xF2)");
                    CIRRUS_LOG("WMFW Block Start Offset: 0x%08zX", pos);
                    CIRRUS_LOG("WMFW Block Payload Size: %u bytes", len);
                    if (!parseWMFWAlgorithmData(raw_region->data, len, outImage, pos, outImage->version))
                        return false;
                    break;
                case WMFW_METADATA:
                    reg.regionType = RegionType::METADATA;
                    break;
                case WMFW_INFO_TEXT:
                    reg.regionType = RegionType::INFO_TEXT;
                    break;
                case WMFW_NAME_TEXT:
                    reg.regionType = RegionType::NAME_TEXT;
                    break;
                case WMFW_ADSP2_XM:
                    reg.regionType = RegionType::XM_UNPACKED;
                    break;
                case WMFW_ADSP2_YM:
                    reg.regionType = RegionType::YM_UNPACKED;
                    break;
                default:
                    return false;
                }
            }

            pos += len;
        }

        return true;
    }

    static inline uint32_t readUnpacked32BE(const uint8_t* data, uint32_t wordIdx) {
        const uint32_t b = wordIdx * 4;
        return ((uint32_t)data[b] << 24) | ((uint32_t)data[b + 1] << 16) | ((uint32_t)data[b + 2] << 8) | (uint32_t)data[b + 3];
    }

    static bool parseAlgorithmTable(const uint8_t* vmem, size_t size, FirmwareImage& outImage) {
        outImage.algorithmCount = 0;
        outImage.firmwareId = outImage.haloFirmwareVersion = outImage.algorithmTotal = 0;
        if (!vmem || size < 40)
            return false;
        uint32_t count = readUnpacked32BE(vmem, 9);
        if (!count || count > 31 || size < 40 + size_t(count) * 24)
            return false;
        for (uint32_t i = 0; i <= count; ++i) {
            uint32_t word = i ? 10 + (i - 1) * 6 : 3;
            uint32_t id = readUnpacked32BE(vmem, word);
            if (!id || id > 0xFFFFFF || readUnpacked32BE(vmem, word + 2) > 0xFFFFFF || readUnpacked32BE(vmem, word + 4) > 0xFFFFFF)
                return false;
            for (uint32_t j = 0; j < i; ++j) {
                uint32_t prior = j ? 10 + (j - 1) * 6 : 3;
                if (id == readUnpacked32BE(vmem, prior))
                    return false;
            }
        }
        outImage.firmwareId = readUnpacked32BE(vmem, 3);
        outImage.haloFirmwareVersion = readUnpacked32BE(vmem, 4);
        outImage.algorithmTotal = count;
        for (uint32_t i = 0; i <= count; ++i) {
            uint32_t word = i ? 10 + (i - 1) * 6 : 3;
            auto& alg = outImage.algorithms[i];
            alg.id = readUnpacked32BE(vmem, word);
            alg.version = readUnpacked32BE(vmem, word + 1);
            alg.baseWordOffset = readUnpacked32BE(vmem, word + 2);
            alg.size = readUnpacked32BE(vmem, word + 3);
            alg.ymBaseWordOffset = readUnpacked32BE(vmem, word + 4);
            alg.region = RegionType::XM_PACKED;
        }
        outImage.algorithmCount = count + 1;
        return true;
    }

    static bool parseBIN(const uint8_t* data, size_t size, FirmwareImage* outImage) {
        if (!outImage)
            return false;
        outImage->coefficientCount = outImage->totalCoeffPayloadBytes = 0;
        if (!data || size < 16 || size > 0xFFFFFFFFULL || memcmp(data, "WMDR", 4))
            return false;
        uint32_t headerLength = readLE32(data + 4);
        if (headerLength < 16 || headerLength > size)
            return false;

        size_t pos = headerLength;
        uint32_t count = 0;
        while (pos < size) {
            if (size - pos < 20 || ++count > 128)
                return false;
            uint16_t type = readLE16(data + pos + 2);
            if (type != WMFW_ADSP2_XM && type != WMFW_ADSP2_YM && type != WMFW_HALO_XM_PACKED && type != WMFW_HALO_YM_PACKED &&
                type != (WMFW_NAME_TEXT << 8) && type != (WMFW_INFO_TEXT << 8) && type != (WMFW_METADATA << 8))
                return false;
            uint32_t length = readLE32(data + pos + 16);
            pos += 20;
            uint64_t padded = (uint64_t(length) + 3) & ~uint64_t(3);
            if (padded > size - pos)
                return false;
            pos += static_cast<size_t>(padded);
        }
        if (!count)
            return false;
        pos = headerLength;
        for (uint32_t i = 0; i < count; ++i) {
            auto& coeff = outImage->coefficients[i];
            coeff.offset = readLE16(data + pos);
            coeff.type = readLE16(data + pos + 2);
            coeff.id = readLE32(data + pos + 4);
            coeff.version = readLE32(data + pos + 8) >> 8;

            coeff.length = readLE32(data + pos + 16);
            pos += 20;
            coeff.data = data + pos;
            coeff.payloadCrc = calculate_crc32(coeff.data, coeff.length);
            outImage->totalCoeffPayloadBytes += coeff.length;
            pos += (size_t(coeff.length) + 3) & ~size_t(3);
        }
        outImage->coefficientCount = count;
        return true;
    }
};
