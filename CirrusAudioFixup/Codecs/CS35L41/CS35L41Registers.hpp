#ifndef CS35L41Registers_hpp
#define CS35L41Registers_hpp

#include <IOKit/IOLib.h>

#define CS35L41_SW_RESET        0x00000000
#define CS35L41_SW_RESET_VAL    0x00005A00
#define CS35L41_TEST_KEY_CTL    0x00000040
#define CS35L41_IRQ1_STATUS4    0x0001001C
#define CS35L41_OTP_BOOT_DONE   0x00000002

struct RegisterDesc {
    uint32_t addr;
    const char* name;
    bool readable;
    bool volatileReg;
};

static const RegisterDesc cs35l41_reg_desc[] = {
    { 0x00000, "CS35L41_DEVID", true, true },
    { 0x00004, "CS35L41_REVID", true, true },
    { 0x00008, "CS35L41_FABID", true, true },
    { 0x0000C, "CS35L41_RELID", true, false },
    { 0x00010, "CS35L41_OTPID", true, true },
    { 0x00020, "CS35L41_SFT_RESET", true, true },
    { 0x00040, "CS35L41_TEST_KEY_CTL", true, true },
    { 0x00044, "CS35L41_USER_KEY_CTL", true, true },
    { 0x00500, "CS35L41_OTP_CTRL0", true, false },
    { 0x00508, "CS35L41_OTP_CTRL3", true, false },
    { 0x0050C, "CS35L41_OTP_CTRL4", true, false },
    { 0x00510, "CS35L41_OTP_CTRL5", true, false },
    { 0x00514, "CS35L41_OTP_CTRL6", true, false },
    { 0x00518, "CS35L41_OTP_CTRL7", true, false },
    { 0x0051C, "CS35L41_OTP_CTRL8", true, false },
    { 0x02014, "CS35L41_PWR_CTRL1", true, false },
    { 0x02018, "CS35L41_PWR_CTRL2", true, false },
    { 0x0201C, "CS35L41_PWR_CTRL3", true, false },
    { 0x02020, "CS35L41_CTRL_OVRRIDE", true, false },
    { 0x02024, "CS35L41_AMP_OUT_MUTE", true, false },
    { 0x02030, "CS35L41_OTP_TRIM_36", true, false },
    { 0x02034, "CS35L41_PROTECT_REL_ERR_IGN", true, false },
    { 0x0208C, "CS35L41_OTP_TRIM_1", true, false },
    { 0x02090, "CS35L41_OTP_TRIM_2", true, false },
    { 0x0242C, "CS35L41_GPIO_PAD_CONTROL", true, false },
    { 0x02438, "CS35L41_JTAG_CONTROL", true, false },
    { 0x02900, "CS35L41_PWRMGT_CTL", true, true },
    { 0x02904, "CS35L41_WAKESRC_CTL", true, true },
    { 0x02908, "CS35L41_PWRMGT_STS", true, true },
    { 0x02C04, "CS35L41_PLL_CLK_CTRL", true, false },
    { 0x02C08, "CS35L41_DSP_CLK_CTRL", true, false },
    { 0x02C0C, "CS35L41_GLOBAL_CLK_CTRL", true, false },
    { 0x02C10, "CS35L41_DATA_FS_SEL", true, false },
    { 0x02D10, "CS35L41_TST_FS_MON0", true, false },
    { 0x0300C, "CS35L41_OTP_TRIM_4", true, false },
    { 0x03010, "CS35L41_OTP_TRIM_3", true, false },
    { 0x03018, "CS35L41_PLL_OVR", true, false },
    { 0x03400, "CS35L41_MDSYNC_EN", true, false },
    { 0x03408, "CS35L41_MDSYNC_TX_ID", true, false },
    { 0x0340C, "CS35L41_MDSYNC_PWR_CTRL", true, false },
    { 0x03410, "CS35L41_MDSYNC_DATA_TX", true, false },
    { 0x03414, "CS35L41_MDSYNC_TX_STATUS", true, false },
    { 0x0341C, "CS35L41_MDSYNC_DATA_RX", true, false },
    { 0x03420, "CS35L41_MDSYNC_RX_STATUS", true, false },
    { 0x03424, "CS35L41_MDSYNC_ERR_STATUS", true, false },
    { 0x03528, "CS35L41_MDSYNC_SYNC_PTE2", true, false },
    { 0x0352C, "CS35L41_MDSYNC_SYNC_PTE3", true, false },
    { 0x0353C, "CS35L41_MDSYNC_SYNC_MSM_STATUS", true, false },
    { 0x03800, "CS35L41_BSTCVRT_VCTRL1", true, false },
    { 0x03804, "CS35L41_BSTCVRT_VCTRL2", true, false },
    { 0x03808, "CS35L41_BSTCVRT_PEAK_CUR", true, false },
    { 0x0380C, "CS35L41_BSTCVRT_SFT_RAMP", true, false },
    { 0x03810, "CS35L41_BSTCVRT_COEFF", true, false },
    { 0x03814, "CS35L41_BSTCVRT_SLOPE_LBST", true, false },
    { 0x03818, "CS35L41_BSTCVRT_SW_FREQ", true, false },
    { 0x0381C, "CS35L41_BSTCVRT_DCM_CTRL", true, false },
    { 0x03820, "CS35L41_BSTCVRT_DCM_MODE_FORCE", true, false },
    { 0x03830, "CS35L41_BSTCVRT_OVERVOLT_CTRL", true, false },
    { 0x03900, "CS35L41_BST_TEST_DUTY", true, false },
    { 0x0394C, "CS35L41_OTP_TRIM_5", true, false },
    { 0x03950, "CS35L41_OTP_TRIM_6", true, false },
    { 0x03954, "CS35L41_OTP_TRIM_7", true, false },
    { 0x03958, "CS35L41_OTP_TRIM_8", true, false },
    { 0x0395C, "CS35L41_OTP_TRIM_9", true, false },
    { 0x04000, "CS35L41_VI_VOL_POL", true, false },
    { 0x0400C, "CS35L41_OTP_TRIM_35", true, false },
    { 0x0410C, "CS35L41_OTP_TRIM_34", true, false },
    { 0x04160, "CS35L41_OTP_TRIM_11", true, false },
    { 0x0416C, "CS35L41_OTP_TRIM_10", true, false },
    { 0x04170, "CS35L41_OTP_TRIM_12", true, false },
    { 0x04220, "CS35L41_DTEMP_WARN_THLD", true, false },
    { 0x04224, "CS35L41_DTEMP_CFG", true, false },
    { 0x04308, "CS35L41_DTEMP_EN", true, true },
    { 0x04360, "CS35L41_OTP_TRIM_13", true, false },
    { 0x04400, "CS35L41_VPVBST_FS_SEL", true, false },
    { 0x04448, "CS35L41_OTP_TRIM_14", true, false },
    { 0x0444C, "CS35L41_OTP_TRIM_15", true, false },
    { 0x04800, "CS35L41_SP_ENABLES", true, false },
    { 0x04804, "CS35L41_SP_RATE_CTRL", true, false },
    { 0x04808, "CS35L41_SP_FORMAT", true, false },
    { 0x0480C, "CS35L41_SP_HIZ_CTRL", true, false },
    { 0x04810, "CS35L41_SP_FRAME_TX_SLOT", true, false },
    { 0x04820, "CS35L41_SP_FRAME_RX_SLOT", true, false },
    { 0x04830, "CS35L41_SP_TX_WL", true, false },
    { 0x04840, "CS35L41_SP_RX_WL", true, false },
    { 0x04C00, "CS35L41_DAC_PCM1_SRC", true, false },
    { 0x04C20, "CS35L41_ASP_TX1_SRC", true, false },
    { 0x04C24, "CS35L41_ASP_TX2_SRC", true, false },
    { 0x04C28, "CS35L41_ASP_TX3_SRC", true, false },
    { 0x04C2C, "CS35L41_ASP_TX4_SRC", true, false },
    { 0x04C40, "CS35L41_DSP1_RX1_SRC", true, false },
    { 0x04C44, "CS35L41_DSP1_RX2_SRC", true, false },
    { 0x04C48, "CS35L41_DSP1_RX3_SRC", true, false },
    { 0x04C4C, "CS35L41_DSP1_RX4_SRC", true, false },
    { 0x04C50, "CS35L41_DSP1_RX5_SRC", true, false },
    { 0x04C54, "CS35L41_DSP1_RX6_SRC", true, false },
    { 0x04C58, "CS35L41_DSP1_RX7_SRC", true, false },
    { 0x04C5C, "CS35L41_DSP1_RX8_SRC", true, false },
    { 0x04C60, "CS35L41_NGATE1_SRC", true, false },
    { 0x04C64, "CS35L41_NGATE2_SRC", true, false },
    { 0x06000, "CS35L41_AMP_DIG_VOL_CTRL", true, false },
    { 0x06404, "CS35L41_VPBR_CFG", true, false },
    { 0x06408, "CS35L41_VBBR_CFG", true, false },
    { 0x0640C, "CS35L41_VPBR_STATUS", true, false },
    { 0x06410, "CS35L41_VBBR_STATUS", true, false },
    { 0x06414, "CS35L41_OVERTEMP_CFG", true, false },
    { 0x06418, "CS35L41_AMP_ERR_VOL", true, false },
    { 0x06450, "CS35L41_VOL_STATUS_TO_DSP", true, false },
    { 0x06800, "CS35L41_CLASSH_CFG", true, false },
    { 0x06804, "CS35L41_WKFET_CFG", true, false },
    { 0x06808, "CS35L41_NG_CFG", true, false },
    { 0x06C04, "CS35L41_AMP_GAIN_CTRL", true, false },
    { 0x06E30, "CS35L41_OTP_TRIM_16", true, false },
    { 0x06E34, "CS35L41_OTP_TRIM_17", true, false },
    { 0x06E38, "CS35L41_OTP_TRIM_18", true, false },
    { 0x06E3C, "CS35L41_OTP_TRIM_19", true, false },
    { 0x06E40, "CS35L41_OTP_TRIM_20", true, false },
    { 0x06E44, "CS35L41_OTP_TRIM_21", true, false },
    { 0x06E48, "CS35L41_OTP_TRIM_22", true, false },
    { 0x06E4C, "CS35L41_OTP_TRIM_23", true, false },
    { 0x06E50, "CS35L41_OTP_TRIM_24", true, false },
    { 0x06E54, "CS35L41_OTP_TRIM_25", true, false },
    { 0x06E58, "CS35L41_OTP_TRIM_26", true, false },
    { 0x06E5C, "CS35L41_OTP_TRIM_27", true, false },
    { 0x06E60, "CS35L41_OTP_TRIM_28", true, false },
    { 0x06E64, "CS35L41_OTP_TRIM_29", true, false },
    { 0x07068, "CS35L41_OTP_TRIM_33", true, false },
    { 0x0706C, "CS35L41_DIGPWM_IOCTRL", true, false },
    { 0x07400, "CS35L41_DAC_MSM_CFG", true, false },
    { 0x07418, "CS35L41_OTP_TRIM_30", true, false },
    { 0x0741C, "CS35L41_OTP_TRIM_31", true, false },
    { 0x07434, "CS35L41_OTP_TRIM_32", true, false },
    { 0x10000, "CS35L41_IRQ1_CFG", true, false },
    { 0x10004, "CS35L41_IRQ1_STATUS", true, true },
    { 0x10010, "CS35L41_IRQ1_STATUS1", true, true },
    { 0x10014, "CS35L41_IRQ1_STATUS2", true, true },
    { 0x10018, "CS35L41_IRQ1_STATUS3", true, true },
    { 0x1001C, "CS35L41_IRQ1_STATUS4", true, true },
    { 0x10090, "CS35L41_IRQ1_RAW_STATUS1", true, true },
    { 0x10094, "CS35L41_IRQ1_RAW_STATUS2", true, true },
    { 0x10098, "CS35L41_IRQ1_RAW_STATUS3", true, true },
    { 0x1009C, "CS35L41_IRQ1_RAW_STATUS4", true, true },
    { 0x10110, "CS35L41_IRQ1_MASK1", true, false },
    { 0x10114, "CS35L41_IRQ1_MASK2", true, false },
    { 0x10118, "CS35L41_IRQ1_MASK3", true, false },
    { 0x1011C, "CS35L41_IRQ1_MASK4", true, false },
    { 0x10190, "CS35L41_IRQ1_FRC1", true, false },
    { 0x10194, "CS35L41_IRQ1_FRC2", true, false },
    { 0x10198, "CS35L41_IRQ1_FRC3", true, false },
    { 0x1019C, "CS35L41_IRQ1_FRC4", true, false },
    { 0x10210, "CS35L41_IRQ1_EDGE1", true, false },
    { 0x1021C, "CS35L41_IRQ1_EDGE4", true, false },
    { 0x10290, "CS35L41_IRQ1_POL1", true, false },
    { 0x10294, "CS35L41_IRQ1_POL2", true, false },
    { 0x10298, "CS35L41_IRQ1_POL3", true, false },
    { 0x1029C, "CS35L41_IRQ1_POL4", true, false },
    { 0x10318, "CS35L41_IRQ1_DB3", true, false },
    { 0x10800, "CS35L41_IRQ2_CFG", true, false },
    { 0x10804, "CS35L41_IRQ2_STATUS", true, true },
    { 0x10810, "CS35L41_IRQ2_STATUS1", true, true },
    { 0x10814, "CS35L41_IRQ2_STATUS2", true, true },
    { 0x10818, "CS35L41_IRQ2_STATUS3", true, true },
    { 0x1081C, "CS35L41_IRQ2_STATUS4", true, true },
    { 0x10890, "CS35L41_IRQ2_RAW_STATUS1", true, true },
    { 0x10894, "CS35L41_IRQ2_RAW_STATUS2", true, true },
    { 0x10898, "CS35L41_IRQ2_RAW_STATUS3", true, true },
    { 0x1089C, "CS35L41_IRQ2_RAW_STATUS4", true, true },
    { 0x10910, "CS35L41_IRQ2_MASK1", true, false },
    { 0x10914, "CS35L41_IRQ2_MASK2", true, false },
    { 0x10918, "CS35L41_IRQ2_MASK3", true, false },
    { 0x1091C, "CS35L41_IRQ2_MASK4", true, false },
    { 0x10990, "CS35L41_IRQ2_FRC1", true, false },
    { 0x10994, "CS35L41_IRQ2_FRC2", true, false },
    { 0x10998, "CS35L41_IRQ2_FRC3", true, false },
    { 0x1099C, "CS35L41_IRQ2_FRC4", true, false },
    { 0x10A10, "CS35L41_IRQ2_EDGE1", true, false },
    { 0x10A1C, "CS35L41_IRQ2_EDGE4", true, false },
    { 0x10A90, "CS35L41_IRQ2_POL1", true, false },
    { 0x10A94, "CS35L41_IRQ2_POL2", true, false },
    { 0x10A98, "CS35L41_IRQ2_POL3", true, false },
    { 0x10A9C, "CS35L41_IRQ2_POL4", true, false },
    { 0x10B18, "CS35L41_IRQ2_DB3", true, false },
    { 0x11000, "CS35L41_GPIO_STATUS1", true, true },
    { 0x11008, "CS35L41_GPIO1_CTRL1", true, false },
    { 0x1100C, "CS35L41_GPIO2_CTRL1", true, false },
    { 0x12000, "CS35L41_MIXER_NGATE_CFG", true, false },
    { 0x12004, "CS35L41_MIXER_NGATE_CH1_CFG", true, false },
    { 0x12008, "CS35L41_MIXER_NGATE_CH2_CFG", true, false },
    { 0x14000, "CS35L41_CLOCK_DETECT_1", true, false },
    { 0x17040, "CS35L41_DIE_STS1", true, false },
    { 0x17044, "CS35L41_DIE_STS2", true, false },
    { 0x17048, "CS35L41_TEMP_CAL1", true, false },
    { 0x1704C, "CS35L41_TEMP_CAL2", true, false },
    { 0x25C0800, "CS35L41_DSP1_TIMESTAMP_COUNT", true, false },
    { 0x25E0000, "CS35L41_DSP1_SYS_ID", true, false },
    { 0x25E0004, "CS35L41_DSP1_SYS_VERSION", true, false },
    { 0x25E0008, "CS35L41_DSP1_SYS_CORE_ID", true, false },
    { 0x25E000C, "CS35L41_DSP1_SYS_AHB_ADDR", true, false },
    { 0x25E0010, "CS35L41_DSP1_SYS_XSRAM_SIZE", true, false },
    { 0x25E0018, "CS35L41_DSP1_SYS_YSRAM_SIZE", true, false },
    { 0x25E0020, "CS35L41_DSP1_SYS_PSRAM_SIZE", true, false },
    { 0x25E0028, "CS35L41_DSP1_SYS_PM_BOOT_SIZE", true, false },
    { 0x25E002C, "CS35L41_DSP1_SYS_FEATURES", true, false },
    { 0x25E0030, "CS35L41_DSP1_SYS_FIR_FILTERS", true, false },
    { 0x25E0034, "CS35L41_DSP1_SYS_LMS_FILTERS", true, false },
    { 0x25E0038, "CS35L41_DSP1_SYS_XM_BANK_SIZE", true, false },
    { 0x25E003C, "CS35L41_DSP1_SYS_YM_BANK_SIZE", true, false },
    { 0x25E0040, "CS35L41_DSP1_SYS_PM_BANK_SIZE", true, false },
    { 0x2B80080, "CS35L41_DSP1_RX1_RATE", true, false },
    { 0x2B80088, "CS35L41_DSP1_RX2_RATE", true, false },
    { 0x2B80090, "CS35L41_DSP1_RX3_RATE", true, false },
    { 0x2B80098, "CS35L41_DSP1_RX4_RATE", true, false },
    { 0x2B800A0, "CS35L41_DSP1_RX5_RATE", true, false },
    { 0x2B800A8, "CS35L41_DSP1_RX6_RATE", true, false },
    { 0x2B800B0, "CS35L41_DSP1_RX7_RATE", true, false },
    { 0x2B800B8, "CS35L41_DSP1_RX8_RATE", true, false },
    { 0x2B80280, "CS35L41_DSP1_TX1_RATE", true, false },
    { 0x2B80288, "CS35L41_DSP1_TX2_RATE", true, false },
    { 0x2B80290, "CS35L41_DSP1_TX3_RATE", true, false },
    { 0x2B80298, "CS35L41_DSP1_TX4_RATE", true, false },
    { 0x2B802A0, "CS35L41_DSP1_TX5_RATE", true, false },
    { 0x2B802A8, "CS35L41_DSP1_TX6_RATE", true, false },
    { 0x2B802B0, "CS35L41_DSP1_TX7_RATE", true, false },
    { 0x2B802B8, "CS35L41_DSP1_TX8_RATE", true, false },
    { 0x2B805C0, "CS35L41_DSP1_SCRATCH1", true, true },
    { 0x2B805C8, "CS35L41_DSP1_SCRATCH2", true, true },
    { 0x2B805D0, "CS35L41_DSP1_SCRATCH3", true, true },
    { 0x2B805D8, "CS35L41_DSP1_SCRATCH4", true, true },
    { 0x2BC1000, "CS35L41_DSP1_CCM_CORE_CTRL", true, false },
    { 0x2BC1008, "CS35L41_DSP1_CCM_CLK_OVERRIDE", true, false },
    { 0x2BC2000, "CS35L41_DSP1_XM_MSTR_EN", true, false },
    { 0x2BC2008, "CS35L41_DSP1_XM_CORE_PRI", true, false },
    { 0x2BC2010, "CS35L41_DSP1_XM_AHB_PACK_PL_PRI", true, false },
    { 0x2BC2018, "CS35L41_DSP1_XM_AHB_UP_PL_PRI", true, false },
    { 0x2BC2020, "CS35L41_DSP1_XM_ACCEL_PL0_PRI", true, false },
    { 0x2BC2078, "CS35L41_DSP1_XM_NPL0_PRI", true, false },
    { 0x2BC20C0, "CS35L41_DSP1_YM_MSTR_EN", true, false },
    { 0x2BC20C8, "CS35L41_DSP1_YM_CORE_PRI", true, false },
    { 0x2BC20D0, "CS35L41_DSP1_YM_AHB_PACK_PL_PRI", true, false },
    { 0x2BC20D8, "CS35L41_DSP1_YM_AHB_UP_PL_PRI", true, false },
    { 0x2BC20E0, "CS35L41_DSP1_YM_ACCEL_PL0_PRI", true, false },
    { 0x2BC2138, "CS35L41_DSP1_YM_NPL0_PRI", true, false },
    { 0x2BC3000, "CS35L41_DSP1_MPU_XM_ACCESS0", true, false },
    { 0x2BC3004, "CS35L41_DSP1_MPU_YM_ACCESS0", true, false },
    { 0x2BC3008, "CS35L41_DSP1_MPU_WNDW_ACCESS0", true, false },
    { 0x2BC300C, "CS35L41_DSP1_MPU_XREG_ACCESS0", true, false },
    { 0x2BC3014, "CS35L41_DSP1_MPU_YREG_ACCESS0", true, false },
    { 0x2BC3018, "CS35L41_DSP1_MPU_XM_ACCESS1", true, false },
    { 0x2BC301C, "CS35L41_DSP1_MPU_YM_ACCESS1", true, false },
    { 0x2BC3020, "CS35L41_DSP1_MPU_WNDW_ACCESS1", true, false },
    { 0x2BC3024, "CS35L41_DSP1_MPU_XREG_ACCESS1", true, false },
    { 0x2BC302C, "CS35L41_DSP1_MPU_YREG_ACCESS1", true, false },
    { 0x2BC3030, "CS35L41_DSP1_MPU_XM_ACCESS2", true, false },
    { 0x2BC3034, "CS35L41_DSP1_MPU_YM_ACCESS2", true, false },
    { 0x2BC3038, "CS35L41_DSP1_MPU_WNDW_ACCESS2", true, false },
    { 0x2BC303C, "CS35L41_DSP1_MPU_XREG_ACCESS2", true, false },
    { 0x2BC3044, "CS35L41_DSP1_MPU_YREG_ACCESS2", true, false },
    { 0x2BC3048, "CS35L41_DSP1_MPU_XM_ACCESS3", true, false },
    { 0x2BC304C, "CS35L41_DSP1_MPU_YM_ACCESS3", true, false },
    { 0x2BC3050, "CS35L41_DSP1_MPU_WNDW_ACCESS3", true, false },
    { 0x2BC3054, "CS35L41_DSP1_MPU_XREG_ACCESS3", true, false },
    { 0x2BC305C, "CS35L41_DSP1_MPU_YREG_ACCESS3", true, false },
    { 0x2BC3100, "CS35L41_DSP1_MPU_XM_VIO_ADDR", true, false },
    { 0x2BC3104, "CS35L41_DSP1_MPU_XM_VIO_STATUS", true, false },
    { 0x2BC3108, "CS35L41_DSP1_MPU_YM_VIO_ADDR", true, false },
    { 0x2BC310C, "CS35L41_DSP1_MPU_YM_VIO_STATUS", true, false },
    { 0x2BC3110, "CS35L41_DSP1_MPU_PM_VIO_ADDR", true, false },
    { 0x2BC3114, "CS35L41_DSP1_MPU_PM_VIO_STATUS", true, false },
    { 0x2BC3140, "CS35L41_DSP1_MPU_LOCK_CONFIG", true, false },
    { 0x2BC3180, "CS35L41_DSP1_MPU_WDT_RST_CTRL", true, false },
};

#define CS35L41_PLL_CLK_CTRL        0x00002C04
#define CS35L41_DSP_CLK_CTRL        0x00002C08
#define CS35L41_GLOBAL_CLK_CTRL     0x00002C0C
#define CS35L41_SP_RATE_CTRL        0x00004804
#define CS35L41_SP_FORMAT           0x00004808
#define CS35L41_SP_TX_WL            0x00004810
#define CS35L41_SP_RX_WL            0x00004814
#define CS35L41_DAC_PCM1_SRC        0x00004C00
#define CS35L41_ASP_TX1_SRC         0x00004C20
#define CS35L41_ASP_TX2_SRC         0x00004C24
#define CS35L41_ASP_TX3_SRC         0x00004C28
#define CS35L41_ASP_TX4_SRC         0x00004C2C
#define CS35L41_DSP1_RX1_SRC        0x00004C40
#define CS35L41_DSP1_RX2_SRC        0x00004C44
#define CS35L41_DSP1_RX3_SRC        0x00004C48
#define CS35L41_DSP1_RX4_SRC        0x00004C4C
#define CS35L41_DSP1_RX6_SRC        0x00004C54
#define CS35L41_SP_HIZ_CTRL         0x0000480C
#define CS35L41_SP_ENABLES          0x00004800
#define CS35L41_GPIO1_CTRL1         0x00011008
#define CS35L41_GPIO2_CTRL1         0x0001100C
#define CS35L41_GPIO_PAD_CONTROL    0x0000242C

#define CS35L41_DSP1_CCM_CORE_CTRL      0x02BC1000
#define CS35L41_DSP1_SYS_ID             0x025E0000
#define CS35L41_DSP1_SYS_VERSION        0x025E0004
#define CS35L41_DSP1_SYS_CORE_ID        0x025E0008
#define CS35L41_DSP_MBOX_1              0x00013000
#define CS35L41_DSP_MBOX_2              0x00013004
#define HALO_CORE_EN                    0x00000001
#define HALO_CORE_RESET                 0x00000200
#define CS35L41_DSP1_MPU_LOCK_CONFIG    0x02BC3140
#define CS35L41_DSP1_MPU_XM_ACCESS0     0x02BC3000
#define CS35L41_DSP1_MPU_YM_ACCESS0     0x02BC3004
#define CS35L41_DSP1_MPU_WNDW_ACCESS0   0x02BC3008
#define CS35L41_DSP1_MPU_XREG_ACCESS0   0x02BC300C
#define CS35L41_DSP1_MPU_YREG_ACCESS0   0x02BC3014
#define CS35L41_DSP1_MPU_XM_ACCESS1     0x02BC3018
#define CS35L41_DSP1_MPU_YM_ACCESS1     0x02BC301C
#define CS35L41_DSP1_MPU_WNDW_ACCESS1   0x02BC3020
#define CS35L41_DSP1_MPU_XREG_ACCESS1   0x02BC3024
#define CS35L41_DSP1_MPU_YREG_ACCESS1   0x02BC302C
#define CS35L41_DSP1_MPU_XM_ACCESS2     0x02BC3030
#define CS35L41_DSP1_MPU_YM_ACCESS2     0x02BC3034
#define CS35L41_DSP1_MPU_WNDW_ACCESS2   0x02BC3038
#define CS35L41_DSP1_MPU_XREG_ACCESS2   0x02BC303C
#define CS35L41_DSP1_MPU_YREG_ACCESS2   0x02BC3044
#define CS35L41_DSP1_MPU_XM_ACCESS3     0x02BC3048
#define CS35L41_DSP1_MPU_YM_ACCESS3     0x02BC304C
#define CS35L41_DSP1_MPU_WNDW_ACCESS3   0x02BC3050
#define CS35L41_DSP1_MPU_XREG_ACCESS3   0x02BC3054
#define CS35L41_DSP1_MPU_YREG_ACCESS3   0x02BC305C

#endif
