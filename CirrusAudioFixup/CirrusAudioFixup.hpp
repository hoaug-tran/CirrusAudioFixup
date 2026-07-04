//
//  CirrusAudioFixup.hpp
//  CirrusAudioFixup
//

#ifndef CirrusAudioFixup_hpp
#define CirrusAudioFixup_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <os/log.h>

#include "Codecs/CS35L41/CS35L41Registers.hpp"
#include "Codecs/CS35L41/CS35L41_OTP.hpp"

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

struct CS35L41Amp {
    const char *name;
    UInt8 address;
    bool present;
    UInt32 deviceId;
    UInt32 revisionId;
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
    IOService *mProvider { nullptr };
    IOWorkLoop *mWorkLoop { nullptr };
    IOTimerEventSource *mProbeTimer { nullptr };

    CS35L41Amp mAmps[2] {
        { "left",  CS35L41_I2C_ADDR_LEFT,  false, 0, 0 },
        { "right", CS35L41_I2C_ADDR_RIGHT, false, 0, 0 },
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
                           
    bool bulkRead(CS35L41Amp &amp, UInt32 reg, UInt8 *data, size_t length, TraceSource source = TRACE_OTHER);
    bool bulkWrite(CS35L41Amp &amp, UInt32 reg, const UInt8 *data, size_t length, TraceSource source = TRACE_OTHER);
    bool readRegister(CS35L41Amp &amp, UInt32 reg, UInt32 *value, TraceSource source = TRACE_OTHER);
    bool writeRegister(CS35L41Amp &amp, UInt32 reg, UInt32 value, TraceSource source = TRACE_OTHER);
    bool updateRegisterBits(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source = TRACE_OTHER);
    bool pollRegisterBit(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs, TraceSource source = TRACE_OTHER);

    bool cs35l41_init_mac(CS35L41Amp &amp);
    
    // Phase 4A.2A: Test Key Unlock/Lock
    bool cs35l41_test_key_unlock(CS35L41Amp &amp);
    bool cs35l41_test_key_lock(CS35L41Amp &amp);
    
    // Phase 4A.2B: Apply Errata
    bool cs35l41_register_errata_patch(CS35L41Amp &amp);
    
    // Phase 4A.2C: OTP Unpack
    bool cs35l41_otp_unpack(CS35L41Amp &amp);
    
    bool cs35l41_apply_phase4A2(CS35L41Amp &amp);
    
    void dumpAllRegisters(CS35L41Amp &amp);
    void testRegisterConsistency(CS35L41Amp &amp);
    void runTimeBasedFSMCheck(CS35L41Amp &amp);
    uint32_t calculateRegistersCRC32(CS35L41Amp &amp);

    // Phase 4A.3: Register Diff Verification
    void snapshotRegisters(CS35L41Amp &amp, UInt32 *snapshot);
    void compareRegisterSnapshots(CS35L41Amp &amp, const UInt32 *oldSnapshot, const UInt32 *newSnapshot);

    static void probeTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* CirrusAudioFixup_hpp */
