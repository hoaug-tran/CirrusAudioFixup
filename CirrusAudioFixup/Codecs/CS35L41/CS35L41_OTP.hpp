#ifndef CS35L41_OTP_HPP
#define CS35L41_OTP_HPP

#include <IOKit/IOLib.h>

struct ErrataPatch {
    UInt32 reg;
    UInt32 value;
};

struct ErrataTable {
    UInt8 revid;
    const ErrataPatch *patches;
    size_t numPatches;
};

// --- B2 Errata Patch from Linux ---
static const ErrataPatch cs35l41_revb2_errata_patch[] = {
    { 0x00004100, 0x00000000 }, // CS35L41_VIMON_SPKMON_RESYNC
    { 0x00004310, 0x00000000 }, // 0x4310
    { 0x00004400, 0x00000000 }, // CS35L41_VPVBST_FS_SEL
    { 0x0000381C, 0x00000051 }, // CS35L41_BSTCVRT_DCM_CTRL
    { 0x02BC20E0, 0x00000000 }, // CS35L41_DSP1_YM_ACCEL_PL0_PRI
    { 0x02BC2020, 0x00000000 }, // CS35L41_DSP1_XM_ACCEL_PL0_PRI
    { 0x00002018, 0x00000000 }, // CS35L41_PWR_CTRL2
    { 0x00006C04, 0x00000000 }, // CS35L41_AMP_GAIN_CTRL
    { 0x00004C28, 0x00000000 }, // CS35L41_ASP_TX3_SRC
    { 0x00004C2C, 0x00000000 }, // CS35L41_ASP_TX4_SRC
};

// We will add OTP maps in Phase 4A.2C

#endif /* CS35L41_OTP_HPP */
