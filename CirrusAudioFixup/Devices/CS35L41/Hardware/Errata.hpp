#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::devices::cs35l41 {

struct ErrataPatch {
    uint32_t reg;
    uint32_t value;
};

struct ErrataTable {
    uint8_t revid;
    const ErrataPatch* patches;
    size_t numPatches;
};

static const ErrataPatch kCs35l41RevB2ErrataPatch[] = {
    {0x00004100, 0x00000000}, {0x00004310, 0x00000000}, {0x00004400, 0x00000000}, {0x0000381C, 0x00000051}, {0x02BC20E0, 0x00000000},
    {0x02BC2020, 0x00000000}, {0x00002018, 0x00000000}, {0x00006C04, 0x00000000}, {0x00004C28, 0x00000000}, {0x00004C2C, 0x00000000},
};

#define cs35l41_revb2_errata_patch kCs35l41RevB2ErrataPatch

}

using ErrataPatch = cirrus::devices::cs35l41::ErrataPatch;
using ErrataTable = cirrus::devices::cs35l41::ErrataTable;
using cirrus::devices::cs35l41::kCs35l41RevB2ErrataPatch;
