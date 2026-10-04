#pragma once

#include "Core/RegisterIO.hpp"
#include "Devices/CS35L41/CS35L41Device.hpp"
#include "Devices/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Firmware/WMFW/FirmwareUploader.hpp"
#include "Platform/HDA/HDAController.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSCollectionIterator.h>

#include <os/log.h>

#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"

// VoodooI2C transfer request packet layout expected by VoodooI2CDeviceNub
struct VoodooI2CAddressedTransfer {
    uint8_t address;
    uint8_t* writeBuffer;
    uint16_t writeLength;
    uint8_t* readBuffer;
    uint16_t readLength;
};

// Sequence table entry for batch register writes, bit updates, and delays
struct RegisterSequence {
    uint32_t reg;
    uint32_t mask;
    uint32_t value;
    uint32_t delay_us;
    bool updateBits;
};

// Hardware state and telemetry tracking for a single amplifier channel (left or right)
struct AmplifierState {
    const char* name;
    uint8_t address;
    bool present;
    uint32_t deviceId;
    uint32_t revisionId;

    const uint8_t* wmfwData;
    size_t wmfwSize;
    const uint8_t* binData;
    size_t binSize;
    bool firmwareValidated;
    uint32_t finalCrc;

    unsigned int monitorCount;
    bool initialized;
    bool dspAlive;

    uint32_t lastIrq1Status1;
    uint32_t lastIrq1Status3;
    uint32_t lastPowerManagementStatus;
    uint32_t lastMailbox2;
    uint32_t lastStreamArbitrationError;
    uint32_t lastClockDetect;
    uint32_t lastStreamArbitrationControl;

    uint32_t lastTimestamp;
    bool playbackActive;
    uint32_t playbackStableCount;

    struct InterestingControl {
        char name[64];
        uint32_t address;
    } diagnosticControls[14];
    uint32_t diagnosticControlCount;
    uint32_t firmwareIdVersion{0};
    uint32_t monitorLogCountdown{0};
    uint32_t haloStateRegister{0};
    uint32_t haloHeartbeatRegister{0};
    bool playbackFaulted{false};
    uint32_t cleanupAttempts{0};
    cirrus::diagnostics::DiagnosticState diagnostic;
};

struct FirmwareImage;

// Firmware binary descriptor mapped to vendor SSID and speaker subsystem ID
struct FirmwareResource {
    uint32_t subsystemVendor;
    uint32_t subsystemDevice;
    uint32_t spkid;
    const char* fwName;
    const char* binName;
    const uint8_t* wmfw;
    size_t wmfwSize;
    const uint8_t* bin;
    size_t binSize;
    const uint8_t* binRight;
    size_t binRightSize;
    bool isDummy;
};

class CirrusAudioFixup : public IOService {
    friend class FixupRegisterIOAdapter;

OSDeclareDefaultStructors(CirrusAudioFixup)

    public : bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService* device) override;

private:
    // Orchestrates full bringup: soft reset, wait for OTP boot, silicon errata, clocks, and DSP
    void fullDriverFlow();

    // Watchdog checking DSP alive status, PLL lock, and latching faults
    void runBackgroundMonitor();

    IOService* mProvider{nullptr};
    IOWorkLoop* mWorkLoop{nullptr};
    IOTimerEventSource* mProbeTimer{nullptr};
    IOCommandGate* mCommandGate{nullptr};
    bool mPMInitialized{false};
    IOPMPowerState mPowerStates[2]{};
    bool mPowerAvailable{true};
    bool mStopping{false};
    bool mNeedsReinitialization{false};

    bool setupPowerManagement(IOService* provider);
    void handlePowerChange(bool powered);
    static IOReturn lifecycleAction(OSObject* owner, void* operation, void*, void*, void*);

    cirrus::platform::hda::HDAStreamState mHdaState;

    IOReturn mLastTransferReturn{kIOReturnSuccess};
    bool mCapturingFailureSnapshot{false};

    AmplifierState mAmps[2]{{"left", cirrus::devices::cs35l41::registers::kI2cAddressLeft},
                            {"right", cirrus::devices::cs35l41::registers::kI2cAddressRight}};

    static const size_t kTraceBufferSize = 1024;
    cirrus::diagnostics::TraceEntry mTraceBuffer[kTraceBufferSize];
    uint32_t mTraceHead{0};
    uint32_t mTraceTail{0};
    cirrus::diagnostics::TraceStats mTraceStats{0};
    IOLock* mTraceLock{nullptr};

    void initTraceBuffer();
    void recordTrace(cirrus::diagnostics::TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen,
                     IOReturn ret);
    void dumpTraceBuffer(const char* propertyName = "Cirrus_Trace_Dump", const char* mirrorPropertyName = nullptr);
    void publishStatistics();
    void setDiagnosticStage(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void markDiagnosticSuccess(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void recordDiagnosticFailure(AmplifierState& amp, cirrus::diagnostics::DiagnosticFailure failure, uint32_t reg = 0,
                                 uint32_t expected = 0, uint32_t actual = 0, IOReturn ioReturn = kIOReturnSuccess,
                                 bool captureSnapshot = true);
    void captureFailureSnapshot(AmplifierState& amp, cirrus::diagnostics::DiagnosticFailure failure);
    void publishDriverVerdict();
    static const char* stageName(cirrus::diagnostics::DriverStage stage);
    static const char* failureName(cirrus::diagnostics::DiagnosticFailure failure);

    bool bootArgEnabled(const char* name);
    bool bootArgStrEquals(const char* name, const char* expectedVal);
    void logProviderInfo(IOService* provider);
    void dumpProviderProperties(IOService* provider);
    bool setupProbeTimer();
    void scheduleReadOnlyProbe(uint32_t delayMs);
    void runReadOnlyProbe();
    void probeAmp(AmplifierState& amp);

    // VoodooI2C transfer chunk size is capped at 252 bytes to prevent controller FIFO overrun
    bool transferToAddress(uint8_t address, uint8_t* writeBuffer, uint16_t writeLength, uint8_t* readBuffer, uint16_t readLength);

public:
    bool bulkWrite(AmplifierState& amp, uint32_t reg, const uint8_t* data, size_t length,
                   cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool bulkRead(AmplifierState& amp, uint32_t reg, uint8_t* data, size_t length,
                  cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

private:
    bool readRegister(AmplifierState& amp, uint32_t reg, uint32_t* value,
                      cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool writeRegister(AmplifierState& amp, uint32_t reg, uint32_t value,
                       cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool updateRegisterBits(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t value,
                            cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool pollRegisterBit(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs,
                         cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

    // Writes mailbox command word and polls for status ACK (pause, resume, spk-protect)
    bool sendMailboxCommand(AmplifierState& amp, uint32_t command, uint32_t expectedStatus);

    void logASPSnapshot(AmplifierState& amp);
    void logDSPSnapshot(AmplifierState& amp);
    void logDSPBootReport(AmplifierState& amp);
    void logPowerSnapshot(AmplifierState& amp);
    void snapshotPlayback(AmplifierState& amp);
    void snapshotDiagnostics(AmplifierState& amp, const char* stage);

    // Hardware reset and revision errata initialization
    bool initCodec(AmplifierState& amp);

    // Test keys unlock protected engineering registers before patching errata
    bool unlockTestKey(AmplifierState& amp);
    bool lockTestKey(AmplifierState& amp);

    // Silicon revision errata matching Linux cs35l41 driver
    bool applyErrataPatch(AmplifierState& amp);

    // Reads OTP memory map to trim analog speaker circuitry
    bool unpackOTP(AmplifierState& amp);

    bool initializeHardwareErrata(AmplifierState& amp);
    void dumpAllRegisters(AmplifierState& amp);
    bool configureHardware(AmplifierState& amp);

    // Snoops AppleHDA stream state to align sample rate and audio format with CS35L41 ASP
    bool syncAlc287HdaCodec();
    bool synchronizeHdaStream() { return syncAlc287HdaCodec(); }
    bool synchronizeHdaCodec() { return syncAlc287HdaCodec(); }
    static bool supportedHdaFormat(uint16_t format);

    void discoverFirmware(AmplifierState& amp);

    // Uploads firmware + tuning bin, releases DSP reset, and verifies heartbeat
    bool bringupDSP(AmplifierState& amp);
    bool bootDsp(AmplifierState& amp) { return bringupDSP(amp); }

    void startBringup() { fullDriverFlow(); }
    bool resetAndInitCodec(AmplifierState& amp) { return initCodec(amp); }
    bool loadOtpCalibration(AmplifierState& amp) { return unpackOTP(amp); }

    // Quiesces audio stream and powers down class D output
    bool stopPlayback(AmplifierState& amp);
    bool checkProtectionStatus(AmplifierState& amp);
    bool verifyDSPAlive(AmplifierState& amp);

    void uploadFirmware(AmplifierState& amp, const char* phaseArg);
    bool parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage);
    bool stopDSP(AmplifierState& amp);

    // Applies speaker calibration (R0) from NVRAM into DSP algorithm memory
    bool applyCalibration(AmplifierState& amp, const FirmwareImage* image);
    void initializeFirmware(AmplifierState& amp, const char* phaseArg);
    void dumpASPRegisters(AmplifierState& amp);

    // Transitions amplifier power state to active class D operation
    bool powerUpAmplifier(AmplifierState& amp);
    bool verifyIdleConfiguration(AmplifierState& amp);

    IOService* audioController();
    IOService* getAudioController();

    void testRegisterConsistency(AmplifierState& amp);
    void runTimeBasedFSMCheck(AmplifierState& amp);

    // Computes CRC-32 checksum across critical hardware registers
    uint32_t calculateRegistersCRC32(AmplifierState& amp);

    void snapshotRegisters(AmplifierState& amp, uint32_t* snapshot);
    void compareRegisterSnapshots(AmplifierState& amp, const uint32_t* oldSnapshot, const uint32_t* newSnapshot);

    bool applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count);

    // Configures internal PLL using SCLK (BCLK) or MCLK reference
    bool applyPLL(AmplifierState& amp);
    bool configurePll(AmplifierState& amp) { return applyPLL(amp); }

    // Sets Audio Serial Port format (I2S, 24/32-bit slot, sample rate)
    bool applyASP(AmplifierState& amp);
    bool configureAsp(AmplifierState& amp) { return applyASP(amp); }

    // Programs GPIO pins for interrupt signaling or boost converter control
    bool applyGPIO(AmplifierState& amp);
    bool configureGpio(AmplifierState& amp) { return applyGPIO(amp); }

    static void probeTimerFired(OSObject* owner, IOTimerEventSource* sender);
};
