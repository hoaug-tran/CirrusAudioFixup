//
//  CirrusAudioFixup.hpp
//  CirrusAudioFixup
//
//  Skeleton kext for Cirrus Logic CS35L41 on Hackintosh
//  Supports: macOS Ventura 13.0+
//  Bundle: com.hoaugtr.CirrusAudioFixup
//

#ifndef CirrusAudioFixup_hpp
#define CirrusAudioFixup_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

// ──────────────────────────────────────────────────────────────
//  Log helpers
// ──────────────────────────────────────────────────────────────
#define LOG_PREFIX     "[CirrusAudioFixup] "
#define CIRRUS_LOG(fmt, ...)  IOLog(LOG_PREFIX fmt "\n", ##__VA_ARGS__)
#define CIRRUS_ERR(fmt, ...)  IOLog(LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)

// ──────────────────────────────────────────────────────────────
//  CS35L41 constants
// ──────────────────────────────────────────────────────────────
#define CS35L41_I2C_ADDR_LEFT   0x40
#define CS35L41_I2C_ADDR_RIGHT  0x41

// ──────────────────────────────────────────────────────────────
//  Driver class declaration
// ──────────────────────────────────────────────────────────────
class CirrusAudioFixup : public IOService {
    OSDeclareDefaultStructors(CirrusAudioFixup)

public:
    // IOKit lifecycle
    virtual bool     init(OSDictionary *properties = nullptr) override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual void     free() override;

private:
    // Whether this instance is the left (0x40) or right (0x41) channel
    bool mIsLeft  { false };
    uint8_t mI2CAddress { 0 };

    // Detect which channel this instance controls via _UID
    void detectChannel(IOService *provider);
};

#endif /* CirrusAudioFixup_hpp */
