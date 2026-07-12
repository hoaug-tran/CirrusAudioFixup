#ifndef CirrusAudioFixup_hpp
#define CirrusAudioFixup_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <os/log.h>

#include "Codecs/CS35L41/Registers.hpp"
#include "Codecs/CS35L41/OTP.hpp"

#define LOG_PREFIX "CirrusAudioFixup: "
#define CIRRUS_LOG(fmt, ...) IOLog(LOG_PREFIX fmt "\n", ##__VA_ARGS__)
#define CIRRUS_ERR(fmt, ...) IOLog(LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)

#define GENMASK(h, l) (((~0U) >> (31 - (h))) & (~0U << (l)))

#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"

#define CS35L41_I2C_ADDR_LEFT   0x40
#define CS35L41_I2C_ADDR_RIGHT  0x41
#define CS35L41_DEVICE_ID       0x00035A40
#define CS35L41_DEVID_REG       0x00000000
#define CS35L41_REVID_REG       0x00000004
#define CS35L41_FABID_REG       0x00000008
#define CS35L41_OTPID_REG       0x00000010
#define CS35L41_PWR_CTRL1_REG   0x00002014
#define CS35L41_PWR_CTRL2_REG   0x00002018
#define CS35L41_PWR_CTRL3_REG   0x0000201C
#define CS35L41_AMP_OUT_MUTE_REG 0x00002024
#define CS35L41_PWRMGT_STS_REG  0x00002908
#define CS35L41_IRQ1_STATUS1_REG 0x00010010
#define CS35L41_IRQ2_STATUS1_REG 0x00010810
#define CS35L41_IRQ1_MASK1_REG   0x00010110
#define CS35L41_IRQ2_MASK1_REG   0x00010910
#define CS35L41_DSP_MBOX_1_REG   0x00013000
#define CS35L41_DSP_MBOX_2_REG   0x00013004
#define CS35L41_DSP_MBOX_3_REG   0x00013008
#define CS35L41_DSP_MBOX_4_REG   0x0001300C

enum TraceSource {
    TRACE_PROBE,
    TRACE_DUMP,
    TRACE_CONSISTENCY,
    TRACE_FIRMWARE,
    TRACE_PLAYBACK,
    TRACE_OTHER
};

struct TraceEntry {
    uint64_t timestamp;
    uint8_t amp;        // 0 for left, 1 for right
    bool isWrite;
    bool isBulk;
    uint32_t reg;
    uint32_t value;     // or length if isBulk
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


struct VoodooI2CAddressedTransfer {
    UInt8 address;
    UInt8 *writeBuffer;
    UInt16 writeLength;
    UInt8 *readBuffer;
    UInt16 readLength;
};

struct RegisterSequence {
    UInt32 reg;
    UInt32 mask;
    UInt32 value;
    UInt32 delay_us;
    bool updateBits;
};

struct CS35L41Amp {
    const char *name;
    UInt8 address;
    bool present;
    UInt32 deviceId;
    UInt32 revisionId;
    
    const uint8_t *wmfwData;
    size_t wmfwSize;
    const uint8_t *binData;
    size_t binSize;
    bool firmwareValidated;
    uint32_t final_crc;
    
    unsigned int monitorCount;
    
    // tracks diagnostic state to avoid log flood on repeating values
    uint32_t last_irq1_sts1;
    uint32_t last_irq1_sts3;
    uint32_t last_pwrmgt_sts;
    uint32_t last_mbox2;
    uint32_t last_strmarb_err;
    uint32_t last_clock_detect;
    uint32_t last_strmarb_ctrl;
    
    struct InterestingControl {
        char name[64];
        uint32_t address;
    } diagnosticControls[10];
    uint32_t diagnosticControlCount;
};

struct FirmwareImage;

struct FirmwareResource {
    uint32_t subsystemVendor;
    uint32_t subsystemDevice;
    uint32_t spkid;
    const char *fwName;
    const char *binName;
    const uint8_t *wmfw;
    size_t wmfwSize;
    const uint8_t *bin;
    size_t binSize;
    bool isDummy;
};

class CirrusAudioFixup : public IOService {
    OSDeclareDefaultStructors(CirrusAudioFixup)

public:
    bool init(OSDictionary *properties = nullptr) override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    void fullDriverFlow();
    
    IOService *mProvider { nullptr };
    IOWorkLoop *mWorkLoop { nullptr };
    IOTimerEventSource *mProbeTimer { nullptr };

    CS35L41Amp mAmps[2] {
        { "left",  CS35L41_I2C_ADDR_LEFT,  false, 0, 0, nullptr, 0, nullptr, 0, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}, 0 },
        { "right", CS35L41_I2C_ADDR_RIGHT, false, 0, 0, nullptr, 0, nullptr, 0, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}, 0 }
    };

    static const size_t kTraceBufferSize = 1024;
    TraceEntry mTraceBuffer[kTraceBufferSize];
    uint32_t mTraceHead { 0 };
    uint32_t mTraceTail { 0 };
    TraceStats mTraceStats { 0 };
    IOLock *mTraceLock { nullptr };

    void initTraceBuffer();
    void recordTrace(TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen, IOReturn ret);
    void dumpTraceBuffer();
    void publishStatistics();

    bool bootArgEnabled(const char *name);
    bool bootArgStrEquals(const char *name, const char *expectedVal);
    void logProviderInfo(IOService *provider);
    void dumpProviderProperties(IOService *provider);
    bool setupProbeTimer();
    void scheduleReadOnlyProbe(UInt32 delayMs);
    void runReadOnlyProbe();
    void probeAmp(CS35L41Amp &amp);

    bool transferToAddress(UInt8 address,
                           UInt8 *writeBuffer,
                           UInt16 writeLength,
                           UInt8 *readBuffer,
                           UInt16 readLength);
                           
public:
    bool bulkWrite(CS35L41Amp &amp, UInt32 reg, const UInt8 *data, size_t length, TraceSource source = TRACE_OTHER);
    bool bulkRead(CS35L41Amp &amp, UInt32 reg, UInt8 *data, size_t length, TraceSource source = TRACE_OTHER);
private:
    bool readRegister(CS35L41Amp &amp, UInt32 reg, UInt32 *value, TraceSource source = TRACE_OTHER);
    bool writeRegister(CS35L41Amp &amp, UInt32 reg, UInt32 value, TraceSource source = TRACE_OTHER);
    bool updateRegisterBits(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source = TRACE_OTHER);
    bool pollRegisterBit(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs, TraceSource source = TRACE_OTHER);

    void logASPSnapshot(CS35L41Amp &amp);
    void logDSPSnapshot(CS35L41Amp &amp);
    void logPowerSnapshot(CS35L41Amp &amp);
    void snapshotPlayback(CS35L41Amp &amp);
    void snapshotDiagnostics(CS35L41Amp &amp, const char* stage);
    
    bool initCodec(CS35L41Amp &amp);
    
    // controls to unlock and lock write access to test registers
    bool unlockTestKey(CS35L41Amp &amp);
    bool lockTestKey(CS35L41Amp &amp);
    
    // custom errata register patches specific to chip revision
    bool applyErrataPatch(CS35L41Amp &amp);
    
    // unpacks factory calibration values from otp memory
    bool unpackOTP(CS35L41Amp &amp);
    
    bool initializeHardwareErrata(CS35L41Amp &amp);
    
    void dumpAllRegisters(CS35L41Amp &amp);
    
    void configureHardware(CS35L41Amp &amp);
    void discoverFirmware(CS35L41Amp &amp);
    void bringupDSP(CS35L41Amp &amp);
    bool verifyDSPAlive(CS35L41Amp &amp);

    // manages loading and uploading dsp firmware binaries
    void uploadFirmware(CS35L41Amp &amp, const char* phaseArg);
    void parseDSPAlgorithms(CS35L41Amp &amp, FirmwareImage &outImage);
    
    void initializeFirmware(CS35L41Amp &amp, const char* phaseArg);
    void dumpASPRegisters(CS35L41Amp &amp);
    void powerUpAmplifier(CS35L41Amp &amp);
    
    IOService* getAudioController();
    
    void testRegisterConsistency(CS35L41Amp &amp);
    void runTimeBasedFSMCheck(CS35L41Amp &amp);
    uint32_t calculateRegistersCRC32(CS35L41Amp &amp);

    // compares register values before and after a change
    void snapshotRegisters(CS35L41Amp &amp, UInt32 *snapshot);
    void compareRegisterSnapshots(CS35L41Amp &amp, const UInt32 *oldSnapshot, const UInt32 *newSnapshot);

    // helper methods to write blocks of config values
    bool applyRegisterSequence(CS35L41Amp &amp, const RegisterSequence* sequence, size_t count);
    bool applyPLL(CS35L41Amp &amp);
    bool applyASP(CS35L41Amp &amp);
    bool applyGPIO(CS35L41Amp &amp);
    
    static void probeTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* CirrusAudioFixup_hpp */
