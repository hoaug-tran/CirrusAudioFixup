#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::firmware::wmfw {

constexpr uint8_t kWmfwMagic0 = 'W';
constexpr uint8_t kWmfwMagic1 = 'M';
constexpr uint8_t kWmfwMagic2 = 'F';
constexpr uint8_t kWmfwMagic3 = 'W';

constexpr uint32_t kWmfwAbsolute = 0xF0;
constexpr uint32_t kWmfwAlgorithmData = 0xF2;
constexpr uint32_t kWmfwMetadata = 0xFC;
constexpr uint32_t kWmfwNameText = 0xFE;
constexpr uint32_t kWmfwInfoText = 0xFF;

constexpr uint32_t kWmfwHaloPmPacked = 0x10;
constexpr uint32_t kWmfwHaloXmPacked = 0x11;
constexpr uint32_t kWmfwHaloYmPacked = 0x12;
constexpr uint32_t kWmfwAdsp2Xm = 0x05;
constexpr uint32_t kWmfwAdsp2Ym = 0x06;

constexpr size_t kMaxFirmwareRegions = 32;
constexpr size_t kMaxMappedRegions = 192;

#pragma pack(push, 1)

struct WmfwHeader {
    char magic[4];
    uint32_t len;
    uint16_t rev;
    uint8_t core;
    uint8_t ver;
};

struct WmfwAdsp2Sizes {
    uint32_t xm;
    uint32_t ym;
    uint32_t pm;
    uint32_t zm;
};

struct WmfwFooter {
    uint64_t timestamp;
    uint32_t checksum;
};

struct WmfwRegion {
    uint32_t type_offset_le;
    uint32_t len;
    uint8_t data[];
};

#pragma pack(pop)

enum class RegionType : uint32_t {
    PmPacked = 0x10,
    XmPacked = 0x11,
    YmPacked = 0x12,
    XmUnpacked = 0x05,
    YmUnpacked = 0x06,
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

}

using wmfw_header = cirrus::firmware::wmfw::WmfwHeader;
using wmfw_adsp2_sizes = cirrus::firmware::wmfw::WmfwAdsp2Sizes;
using wmfw_footer = cirrus::firmware::wmfw::WmfwFooter;
using wmfw_region = cirrus::firmware::wmfw::WmfwRegion;
using RegionType = cirrus::firmware::wmfw::RegionType;
