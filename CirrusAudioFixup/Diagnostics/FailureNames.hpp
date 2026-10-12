#pragma once
#include "Diagnostics/DiagnosticTypes.hpp"
namespace cirrus::diagnostics {
inline const char* failureName(DiagnosticFailure failure) {
    switch (failure) {
    case DIAG_OK:
        return "OK";
    case DIAG_PROVIDER_MISSING:
        return "PROVIDER_MISSING";
    case DIAG_I2C_TRANSFER:
        return "I2C_TRANSFER";
    case DIAG_SPI_TRANSFER:
        return "SPI_TRANSFER";
    case DIAG_RESET_GPIO:
        return "RESET_GPIO";
    case DIAG_DEVICE_ID:
        return "DEVICE_ID";
    case DIAG_RESET_WRITE:
        return "RESET_WRITE";
    case DIAG_OTP_TIMEOUT:
        return "OTP_TIMEOUT";
    case DIAG_OTP_BOOT_ERROR:
        return "OTP_BOOT_ERROR";
    case DIAG_AMP_PROTECTION:
        return "AMP_PROTECTION";
    case DIAG_ERRATA:
        return "ERRATA";
    case DIAG_OTP_UNPACK:
        return "OTP_UNPACK";
    case DIAG_PLL_CONFIG:
        return "PLL_CONFIG";
    case DIAG_ASP_CONFIG:
        return "ASP_CONFIG";
    case DIAG_GPIO_CONFIG:
        return "GPIO_CONFIG";
    case DIAG_PLATFORM_CONFIG:
        return "PLATFORM_CONFIG";
    case DIAG_FIRMWARE_MISSING:
        return "FIRMWARE_MISSING";
    case DIAG_FIRMWARE_PARSE:
        return "FIRMWARE_PARSE";
    case DIAG_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case DIAG_COEFFICIENT_PARSE:
        return "COEFFICIENT_PARSE";
    case DIAG_COEFFICIENT_UPLOAD:
        return "COEFFICIENT_UPLOAD";
    case DIAG_DSP_BOOT:
        return "DSP_BOOT";
    case DIAG_DSP_MAILBOX:
        return "DSP_MAILBOX";
    case DIAG_IDLE_INVARIANT:
        return "IDLE_INVARIANT";
    case DIAG_HDA_CONTROLLER:
        return "HDA_CONTROLLER";
    case DIAG_HDA_STREAM_FORMAT:
        return "HDA_STREAM_FORMAT";
    case DIAG_PLL_UNLOCKED:
        return "PLL_UNLOCKED";
    case DIAG_POWER_UP_TIMEOUT:
        return "POWER_UP_TIMEOUT";
    case DIAG_PLAYBACK_INVARIANT:
        return "PLAYBACK_INVARIANT";
    case DIAG_POWER_DOWN_TIMEOUT:
        return "POWER_DOWN_TIMEOUT";
    case DIAG_IDLE_ROLLBACK:
        return "IDLE_ROLLBACK";
    case DIAG_CALIBRATION:
        return "CALIBRATION";
    default:
        return "UNKNOWN";
    }
}
}
