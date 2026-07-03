//
//  CirrusAudioFixup.hpp
//  CirrusAudioFixup
//

#ifndef CirrusAudioFixup_hpp
#define CirrusAudioFixup_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSCollectionIterator.h>

#define LOG_PREFIX "[CirrusAudioFixup] "
#define CIRRUS_LOG(fmt, ...) IOLog(LOG_PREFIX fmt "\n", ##__VA_ARGS__)
#define CIRRUS_ERR(fmt, ...) IOLog(LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)

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
#define CS35L41_IRQ1_STATUS1_REG 0x00013000

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

    bool bootArgEnabled(const char *name);
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
                           
    bool bulkRead(CS35L41Amp &amp, UInt32 reg, UInt8 *data, size_t length);
    bool bulkWrite(CS35L41Amp &amp, UInt32 reg, const UInt8 *data, size_t length);
    bool readRegister(CS35L41Amp &amp, UInt32 reg, UInt32 *value);
    bool writeRegister(CS35L41Amp &amp, UInt32 reg, UInt32 value);
    void dumpRegisters(CS35L41Amp &amp);

    static void probeTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* CirrusAudioFixup_hpp */
