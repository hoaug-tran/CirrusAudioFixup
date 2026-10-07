#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__APPLE__) && (defined(KERNEL) || defined(_KERNEL) || defined(__KERNEL__))
#include <IOKit/IOTypes.h>
#else
using IOReturn = int;
constexpr int kIOReturnSuccess = 0;
#endif

namespace cirrus::diagnostics {

enum class TraceSource : uint32_t { Probe = 0, Dump, Consistency, Firmware, Playback, Other };

inline constexpr TraceSource TRACE_PROBE = TraceSource::Probe;
inline constexpr TraceSource TRACE_DUMP = TraceSource::Dump;
inline constexpr TraceSource TRACE_CONSISTENCY = TraceSource::Consistency;
inline constexpr TraceSource TRACE_FIRMWARE = TraceSource::Firmware;
inline constexpr TraceSource TRACE_PLAYBACK = TraceSource::Playback;
inline constexpr TraceSource TRACE_OTHER = TraceSource::Other;

enum class DriverStage : uint32_t {
    None = 0,
    Probe,
    Reset,
    OtpBoot,
    Errata,
    Clock,
    Asp,
    Gpio,
    Platform,
    FirmwareDiscovery,
    FirmwareUpload,
    DspBoot,
    IdleVerify,
    HdaDetect,
    PlaybackOpen,
    PlaybackPrepare,
    PlaybackActive,
    PlaybackCleanup,
    PlaybackClose,
    SafeIdle,
};

inline constexpr DriverStage STAGE_NONE = DriverStage::None;
inline constexpr DriverStage STAGE_PROBE = DriverStage::Probe;
inline constexpr DriverStage STAGE_RESET = DriverStage::Reset;
inline constexpr DriverStage STAGE_OTP_BOOT = DriverStage::OtpBoot;
inline constexpr DriverStage STAGE_ERRATA = DriverStage::Errata;
inline constexpr DriverStage STAGE_CLOCK = DriverStage::Clock;
inline constexpr DriverStage STAGE_ASP = DriverStage::Asp;
inline constexpr DriverStage STAGE_GPIO = DriverStage::Gpio;
inline constexpr DriverStage STAGE_PLATFORM = DriverStage::Platform;
inline constexpr DriverStage STAGE_FIRMWARE_DISCOVERY = DriverStage::FirmwareDiscovery;
inline constexpr DriverStage STAGE_FIRMWARE_UPLOAD = DriverStage::FirmwareUpload;
inline constexpr DriverStage STAGE_DSP_BOOT = DriverStage::DspBoot;
inline constexpr DriverStage STAGE_IDLE_VERIFY = DriverStage::IdleVerify;
inline constexpr DriverStage STAGE_HDA_DETECT = DriverStage::HdaDetect;
inline constexpr DriverStage STAGE_PLAYBACK_OPEN = DriverStage::PlaybackOpen;
inline constexpr DriverStage STAGE_PLAYBACK_PREPARE = DriverStage::PlaybackPrepare;
inline constexpr DriverStage STAGE_PLAYBACK_ACTIVE = DriverStage::PlaybackActive;
inline constexpr DriverStage STAGE_PLAYBACK_CLEANUP = DriverStage::PlaybackCleanup;
inline constexpr DriverStage STAGE_PLAYBACK_CLOSE = DriverStage::PlaybackClose;
inline constexpr DriverStage STAGE_SAFE_IDLE = DriverStage::SafeIdle;

enum class DiagnosticFailure : uint32_t {
    Ok = 0,
    ProviderMissing,
    I2cTransfer,
    ResetGpio,
    DeviceId,
    ResetWrite,
    OtpTimeout,
    Errata,
    OtpUnpack,
    PllConfig,
    AspConfig,
    GpioConfig,
    PlatformConfig,
    FirmwareMissing,
    FirmwareParse,
    FirmwareUpload,
    CoefficientParse,
    CoefficientUpload,
    DspBoot,
    DspMailbox,
    IdleInvariant,
    HdaController,
    HdaStreamFormat,
    PllUnlocked,
    PowerUpTimeout,
    PlaybackInvariant,
    PowerDownTimeout,
    IdleRollback,
    OtpBootError,
    AmpProtection,
    Calibration,
};

inline constexpr DiagnosticFailure DIAG_OK = DiagnosticFailure::Ok;
inline constexpr DiagnosticFailure DIAG_PROVIDER_MISSING = DiagnosticFailure::ProviderMissing;
inline constexpr DiagnosticFailure DIAG_I2C_TRANSFER = DiagnosticFailure::I2cTransfer;
inline constexpr DiagnosticFailure DIAG_RESET_GPIO = DiagnosticFailure::ResetGpio;
inline constexpr DiagnosticFailure DIAG_DEVICE_ID = DiagnosticFailure::DeviceId;
inline constexpr DiagnosticFailure DIAG_RESET_WRITE = DiagnosticFailure::ResetWrite;
inline constexpr DiagnosticFailure DIAG_OTP_TIMEOUT = DiagnosticFailure::OtpTimeout;
inline constexpr DiagnosticFailure DIAG_ERRATA = DiagnosticFailure::Errata;
inline constexpr DiagnosticFailure DIAG_OTP_UNPACK = DiagnosticFailure::OtpUnpack;
inline constexpr DiagnosticFailure DIAG_PLL_CONFIG = DiagnosticFailure::PllConfig;
inline constexpr DiagnosticFailure DIAG_ASP_CONFIG = DiagnosticFailure::AspConfig;
inline constexpr DiagnosticFailure DIAG_GPIO_CONFIG = DiagnosticFailure::GpioConfig;
inline constexpr DiagnosticFailure DIAG_PLATFORM_CONFIG = DiagnosticFailure::PlatformConfig;
inline constexpr DiagnosticFailure DIAG_FIRMWARE_MISSING = DiagnosticFailure::FirmwareMissing;
inline constexpr DiagnosticFailure DIAG_FIRMWARE_PARSE = DiagnosticFailure::FirmwareParse;
inline constexpr DiagnosticFailure DIAG_FIRMWARE_UPLOAD = DiagnosticFailure::FirmwareUpload;
inline constexpr DiagnosticFailure DIAG_COEFFICIENT_PARSE = DiagnosticFailure::CoefficientParse;
inline constexpr DiagnosticFailure DIAG_COEFFICIENT_UPLOAD = DiagnosticFailure::CoefficientUpload;
inline constexpr DiagnosticFailure DIAG_DSP_BOOT = DiagnosticFailure::DspBoot;
inline constexpr DiagnosticFailure DIAG_DSP_MAILBOX = DiagnosticFailure::DspMailbox;
inline constexpr DiagnosticFailure DIAG_IDLE_INVARIANT = DiagnosticFailure::IdleInvariant;
inline constexpr DiagnosticFailure DIAG_HDA_CONTROLLER = DiagnosticFailure::HdaController;
inline constexpr DiagnosticFailure DIAG_HDA_STREAM_FORMAT = DiagnosticFailure::HdaStreamFormat;
inline constexpr DiagnosticFailure DIAG_PLL_UNLOCKED = DiagnosticFailure::PllUnlocked;
inline constexpr DiagnosticFailure DIAG_POWER_UP_TIMEOUT = DiagnosticFailure::PowerUpTimeout;
inline constexpr DiagnosticFailure DIAG_PLAYBACK_INVARIANT = DiagnosticFailure::PlaybackInvariant;
inline constexpr DiagnosticFailure DIAG_POWER_DOWN_TIMEOUT = DiagnosticFailure::PowerDownTimeout;
inline constexpr DiagnosticFailure DIAG_IDLE_ROLLBACK = DiagnosticFailure::IdleRollback;
inline constexpr DiagnosticFailure DIAG_OTP_BOOT_ERROR = DiagnosticFailure::OtpBootError;
inline constexpr DiagnosticFailure DIAG_AMP_PROTECTION = DiagnosticFailure::AmpProtection;
inline constexpr DiagnosticFailure DIAG_CALIBRATION = DiagnosticFailure::Calibration;

struct DiagnosticState {
    DriverStage stage{STAGE_NONE};
    DriverStage lastGoodStage{STAGE_NONE};
    DiagnosticFailure firstFailure{DIAG_OK};
    DiagnosticFailure latestFailure{DIAG_OK};
    uint32_t failureCount{0};
    uint32_t reg{0};
    uint32_t expected{0};
    uint32_t actual{0};
    IOReturn ioReturn{kIOReturnSuccess};
    DiagnosticFailure snapshotFailure{DIAG_OK};
};

struct TraceEntry {
    uint64_t timestamp;
    uint8_t amp;
    bool isWrite;
    bool isBulk;
    uint32_t reg;
    uint32_t value;
    IOReturn ret;
    TraceSource source;
};

struct TraceStats {
    uint32_t readSuccess;
    uint32_t readFail;
    uint32_t writeSuccess;
    uint32_t writeFail;
    uint32_t bulkSuccess;
    uint32_t bulkFail;
    uint32_t noackCount;
    uint32_t retries;
};

enum class DriverVerdict : uint32_t {
    Unknown = 0,
    Success,
    HardwareMissing,
    FirmwareMissing,
    OtpTimeout,
    DspBootFailed,
    PllUnlocked,
    BusTransferError,
};

}
