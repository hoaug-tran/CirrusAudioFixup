from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')

def function(name):
    start = re.search(r'(?:bool|void) CirrusAudioFixup::' + name + r'\(', SOURCE).start()
    end = SOURCE.index('\n}', start) + 2
    return SOURCE[start:end]

names = ['checkProtectionStatus', 'stopPlayback', 'runBackgroundMonitor', 'sendMailboxCommand', 'unlockTestKey', 'lockTestKey', 'stopDSP', 'bringupDSP', 'verifyDSPAlive', 'handlePowerChange', 'supportedHdaFormat', 'transferToAddress', 'publishDriverVerdict', 'applyPLL', 'applyASP', 'applyRegisterSequence', 'powerUpAmplifier', 'verifyIdleConfiguration']
functions = '\n'.join(function(name) for name in names)

amp = HEADER[HEADER.index('struct AmplifierState {'):HEADER.index('struct FirmwareImage;')]

LEGACY_CONSTANTS = r'''
#define CS35L41_SW_RESET 0x00000000
#define CS35L41_SW_RESET_VAL 0x00005A00
#define CS35L41_TEST_KEY_CTL 0x00000040
#define CS35L41_IRQ1_STATUS4 0x0001001C
#define CS35L41_OTP_BOOT_DONE 0x00000002
#define CS35L41_IRQ1_STATUS3 0x00010018
#define CS35L41_OTP_BOOT_ERR 0x80000000
#define CS35L41_IRQ1_RAW_STATUS3 0x00010098
#define CS35L41_BST_EN_MASK 0x00000030
#define CS35L41_PROTECTION_MASK 0x800281C0
#define CS35L41_IRQ1_MASK1 0x00010110
#define CS35L41_IRQ1_MASK2 0x00010114
#define CS35L41_IRQ1_MASK3 0x00010118
#define CS35L41_IRQ1_MASK4 0x0001011C
#define CS35L41_IRQ2_MASK1 0x00010910
#define CS35L41_IRQ2_MASK2 0x00010914
#define CS35L41_IRQ2_MASK3 0x00010918
#define CS35L41_IRQ2_MASK4 0x0001091C
#define CS35L41_PLL_CLK_CTRL 0x00002C04
#define CS35L41_DSP_CLK_CTRL 0x00002C08
#define CS35L41_GLOBAL_CLK_CTRL 0x00002C0C
#define CS35L41_SP_RATE_CTRL 0x00004804
#define CS35L41_SP_FORMAT 0x00004808
#define CS35L41_SP_FRAME_TX_SLOT 0x00004810
#define CS35L41_SP_FRAME_RX_SLOT 0x00004820
#define CS35L41_SP_TX_WL 0x00004830
#define CS35L41_SP_RX_WL 0x00004840
#define CS35L41_DAC_PCM1_SRC 0x00004C00
#define CS35L41_ASP_TX1_SRC 0x00004C20
#define CS35L41_ASP_TX2_SRC 0x00004C24
#define CS35L41_ASP_TX3_SRC 0x00004C28
#define CS35L41_ASP_TX4_SRC 0x00004C2C
#define CS35L41_DSP1_RX1_SRC 0x00004C40
#define CS35L41_DSP1_RX2_SRC 0x00004C44
#define CS35L41_DSP1_RX3_SRC 0x00004C48
#define CS35L41_DSP1_RX4_SRC 0x00004C4C
#define CS35L41_DSP1_RX5_SRC 0x00004C50
#define CS35L41_DSP1_RX6_SRC 0x00004C54
#define CS35L41_SP_HIZ_CTRL 0x0000480C
#define CS35L41_SP_ENABLES 0x00004800
#define CS35L41_AMP_DIG_VOL_CTRL 0x00006000
#define CS35L41_AMP_GAIN_CTRL 0x00006C04
#define CS35L41_GPIO1_CTRL1 0x00011008
#define CS35L41_GPIO2_CTRL1 0x0001100C
#define CS35L41_GPIO_PAD_CONTROL 0x0000242C
#define CS35L41_DSP1_RX1_RATE 0x02B80080
#define CS35L41_DSP1_RX2_RATE 0x02B80088
#define CS35L41_DSP1_RX3_RATE 0x02B80090
#define CS35L41_DSP1_RX4_RATE 0x02B80098
#define CS35L41_DSP1_RX5_RATE 0x02B800A0
#define CS35L41_DSP1_RX6_RATE 0x02B800A8
#define CS35L41_DSP1_RX7_RATE 0x02B800B0
#define CS35L41_DSP1_RX8_RATE 0x02B800B8
#define CS35L41_DSP1_TX1_RATE 0x02B80280
#define CS35L41_DSP1_TX2_RATE 0x02B80288
#define CS35L41_DSP1_TX3_RATE 0x02B80290
#define CS35L41_DSP1_TX4_RATE 0x02B80298
#define CS35L41_DSP1_TX5_RATE 0x02B802A0
#define CS35L41_DSP1_TX6_RATE 0x02B802A8
#define CS35L41_DSP1_TX7_RATE 0x02B802B0
#define CS35L41_DSP1_TX8_RATE 0x02B802B8
#define CS35L41_DSP1_CCM_CORE_CTRL 0x02BC1000
#define CS35L41_DSP1_CORE_SOFT_RESET 0x02B80010
#define CS35L41_DSP1_SYS_ID 0x025E0000
#define CS35L41_DSP1_SYS_VERSION 0x025E0004
#define CS35L41_DSP1_SYS_CORE_ID 0x025E0008
#define CS35L41_DSP_MBOX_1 0x00013000
#define CS35L41_DSP_MBOX_2 0x00013004
#define CS35L41_DSP_MBOX_2_REG 0x00013004
#define CS35L41_DSP_VIRT1_MBOX_1 0x00013020
#define CSPL_MBOX_CMD_RESUME 2
#define CSPL_MBOX_CMD_PAUSE 1
#define CSPL_MBOX_CMD_SPK_OUT_ENABLE 7
#define CSPL_MBOX_STS_RUNNING 0
#define CSPL_MBOX_STS_PAUSED 1
#define CSPL_MBOX_STS_RDY_FOR_REINIT 2
#define CS35L41_IRQ1_STATUS1 0x00010010
#define CS35L41_IRQ1_STATUS2 0x00010014
#define CS35L41_IRQ2_STATUS 0x00010804
#define HALO_CORE_EN 0x00000001
#define HALO_CORE_RESET 0x00000200
#define CS35L41_DSP1_MPU_LOCK_CONFIG 0x02BC3140
#define CS35L41_DSP1_MPU_XM_ACCESS0 0x02BC3000
#define CS35L41_DSP1_MPU_YM_ACCESS0 0x02BC3004
#define CS35L41_DSP1_MPU_WND_ACCESS0 0x02BC3008
#define CS35L41_DSP1_MPU_XREG_ACCESS0 0x02BC300C
#define CS35L41_DSP1_MPU_YREG_ACCESS0 0x02BC3010
#define CS35L41_PWR_CTRL1 0x00002014
#define CS35L41_PWR_CTRL1_REG 0x00002014
#define CS35L41_PWR_CTRL2 0x00002018
#define CS35L41_PWR_CTRL2_REG 0x00002018
#define CS35L41_PWR_CTRL3 0x0000201C
#define CS35L41_AMP_OUT_MUTE 0x00002024
#define CS35L41_DEVID_REG 0x00000000
#define CS35L41_REVID_REG 0x00000004
#define CS35L41_FABID_REG 0x00000008
#define CS35L41_OTPID_REG 0x00000010
#define CS35L41_PM_STS_REG 0x00002908
#define CS35L41_DEVICE_ID 0x35A40
'''

preamble = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <map>
#include <vector>
#include <string>
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Platform/HDA/HDAStreamWatcher.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Devices/CS35L41/CS35L41Device.hpp"
using UInt8=uint8_t;
using UInt16=uint16_t;
using UInt32=uint32_t;
using IOReturn=int;
struct RegisterSequence { UInt32 reg,mask,value,delayUs; bool updateBits; };
constexpr int kIOReturnNotReady=-1;
constexpr int kIOReturnBadArgument=-2;
constexpr bool kOSBooleanTrue=true;
#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"
inline void IOSleep(unsigned) {}
inline uint64_t mach_absolute_time() { return 0; }
inline void absolutetime_to_nanoseconds(uint64_t n,uint64_t* p) { *p=n; }
struct OSString {
    std::string value;
    static OSString* withCString(const char* s) { return new OSString{s}; }
    void release() { delete this; }
};
struct Timer { unsigned scheduled=0,cancelled=0; void setTimeoutMS(unsigned) { ++scheduled; } void cancelTimeout() { ++cancelled; } };
struct VoodooI2CAddressedTransfer { UInt8 address; UInt8* writeBuffer; UInt16 writeLength; UInt8* readBuffer; UInt16 readLength; };
struct Provider {
    unsigned calls=0;
    int callPlatformFunction(const char*,bool,void*,void*,void*,void*) { ++calls; return 0; }
};
using IOService=Provider;
namespace cirrus::transport {
class VoodooI2CTransport {
    IOService* provider;
    UInt8 address;
    IOReturn result{kIOReturnNotReady};
public:
    VoodooI2CTransport(IOService* p, UInt8 a) : provider(p), address(a) {}
    bool transfer(UInt8* writeBuffer, UInt16 writeLength, UInt8* readBuffer, UInt16 readLength) {
        if (!provider) { result=kIOReturnNotReady; return false; }
        VoodooI2CAddressedTransfer request{address,writeBuffer,writeLength,readBuffer,readLength};
        result=provider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS,false,&request,nullptr,nullptr,nullptr);
        return result==0;
    }
    IOReturn lastReturn() const { return result; }
};
}
using namespace cirrus::diagnostics;
using namespace cirrus::devices::cs35l41;
bool gCirrusDebug = false;
'''

preamble += amp + '\nusing CS35L41Amp = AmplifierState;\n'

mock = r'''
namespace cirrus { namespace platform { namespace hda {
struct HDAStreamState {
    bool observed{false};
    bool streamActive{false};
    bool converterPrepared{false};
    bool topologyLogged{false};
    uint8_t lastDescriptor{0xFF};
    uint8_t lastStreamTag{0};
    uint16_t lastFormat{0};
    uint32_t missCount{0};
};
}}}

class CirrusAudioFixup {
public:
    size_t mAmpCount{2};
    CS35L41Amp mAmps[2]{};
    Timer* mProbeTimer=nullptr;
    Provider* mProvider=nullptr;
    bool mPowerAvailable=true,mStopping=false,mNeedsReinitialization=false;
    bool mDebugPhaseHalted=false;
    void clearHdaCache() {}
    struct DummyPci { void release() {} };
    struct DummyMap { void release() {} };
    DummyPci* mAudioPciDev=nullptr;
    DummyMap* mAudioBarMap=nullptr;
    volatile uint8_t* mAudioBarBase=nullptr;
    bool mProbingAmplifiers=false;
    cirrus::platform::hda::HDAStreamState mHdaState;
    bool& mHdaConverterPrepared = mHdaState.converterPrepared;
    bool& mHdaControllerObserved = mHdaState.observed;
    bool& mHdaStreamActive = mHdaState.streamActive;
    bool& mHdaTopologyLogged = mHdaState.topologyLogged;
    uint8_t& mHdaLastDescriptor = mHdaState.lastDescriptor;
    uint8_t& mHdaLastStreamTag = mHdaState.lastStreamTag;
    uint16_t& mHdaLastFormat = mHdaState.lastFormat;
    uint32_t& mHdaMissCount = mHdaState.missCount;

    bool readonly=false;
    int mLastTransferReturn=0;
    bool bypass=false,pup=true,pdn=true,pll=true,mailbox=true,halo=true;
    unsigned ops=0,failAt=0,unmute=0,unmuteAfterFault=0;
    unsigned syncCalls=0;
    unsigned restores=0;
    bool endStreamBeforeUnmute=false;
    bool streamEnded=false;
    unsigned startsAfterStreamEnd=0;
    unsigned failAllFrom=0;
    bool corruptRoute=false;
    bool stuckBoost=false;
    bool losePllAtPup=false;
    uint32_t injectProtectionAtPup=0;
    bool failed=false;
    std::map<uint32_t,uint32_t> regs;
    std::map<std::string,std::string> properties;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    CirrusAudioFixup() {
        mHdaConverterPrepared=true; mHdaTopologyLogged=true;
        mHdaControllerObserved=true; mHdaStreamActive=true;
        mHdaLastStreamTag=1;
        auto& a=mAmps[0]; a.name="host"; a.present=true; a.initialized=true;
        a.monitorCount=1; a.firmwareValidated=true; a.dspAlive=true;
        a.haloStateRegister=0x02800398; a.haloHeartbeatRegister=0x0280039C;
        a.firmwareIdVersion=0x1500;
        regs[CS35L41_AMP_DIG_VOL_CTRL]=0xA678;
        regs[CS35L41_DSP_MBOX_2_REG]=CSPL_MBOX_STS_PAUSED;
        regs[CS35L41_DSP1_CCM_CORE_CTRL]=HALO_CORE_EN;
        regs[CS35L41_DSP1_SYS_ID]=1;
        regs[a.haloStateRegister]=2;
    }
    bool step() { ++ops; if(ops==failAt || (failAllFrom && ops>=failAllFrom)) { failed=true; return false; } return true; }
    bool readRegister(CS35L41Amp&,uint32_t r,uint32_t* v,TraceSource=TRACE_OTHER) {
        if(!step()) return false;
        *v=r==0x10098 ? (pll?2:0) : r==0x02800398 ? (halo?2:0) :
            (corruptRoute && r==CS35L41_DAC_PCM1_SRC) ? 0 : regs[r];
        return true;
    }
    bool writeRegister(CS35L41Amp&,uint32_t r,uint32_t v,TraceSource=TRACE_OTHER) {
        writes.push_back({r,v});
        if(r==CS35L41_AMP_DIG_VOL_CTRL && (v==0x8000 || v==0x0000)) { ++unmute; if(failed) ++unmuteAfterFault; }
        if(!step()) return false;
        if(r==0x10010) regs[r]&=~v;
        else if(r==0x2014) {
            bool was=regs[r]&1;
            if((v&1) && !was && streamEnded) ++startsAfterStreamEnd;
            regs[r]=v;
            if((v&1) && !was && pup) regs[0x10010]|=0x1000000|injectProtectionAtPup;
            if((v&1) && !was && losePllAtPup) pll=false;
            if(!(v&1) && was && pdn) regs[0x10010]|=0x800000;
        } else if(r==0x13020) {
            regs[r]=v;
            if(mailbox) regs[0x13004]=v==CSPL_MBOX_CMD_PAUSE?CSPL_MBOX_STS_PAUSED:CSPL_MBOX_STS_RUNNING;
        } else if(r==CS35L41_PWR_CTRL2_REG && stuckBoost) regs[r]=v|0x20;
        else regs[r]=v;
        return true;
    }
    bool pollRegisterBit(CS35L41Amp& a,uint32_t r,uint32_t m,uint32_t exp,uint32_t,TraceSource=TRACE_OTHER) {
        if(!step()) return false;
        uint32_t val = 0;
        if(!readRegister(a, r, &val)) return false;
        return (val & m) == exp;
    }
    bool updateRegisterBits(CS35L41Amp& a,uint32_t r,uint32_t mask,uint32_t v,TraceSource s=TRACE_OTHER) {
        uint32_t old=0;
        return readRegister(a,r,&old,s) && writeRegister(a,r,(old&~mask)|(v&mask),s);
    }
    bool bulkRead(CS35L41Amp&,uint32_t,uint8_t* p,unsigned n,TraceSource=TRACE_OTHER) {
        if(!step()) return false; memset(p,0,n); return true;
    }
    bool synchronizeHdaStream() {
        if(endStreamBeforeUnmute && ++syncCalls>1) { streamEnded=true; mHdaStreamActive=false; }
        return mHdaStreamActive;
    }
    bool bootArgEnabled(const char* name) {
        return (strcmp(name,"-cirrusro")==0) ? readonly :
               (strcmp(name,"-cirrusnodsp")==0) ? bypass : false;
    }
    void setDiagnosticStage(CS35L41Amp&,DriverStage) {}
    template<class... T> void recordDiagnosticFailure(CS35L41Amp& a,DiagnosticFailure f,T...) {
        if(a.diagnostic.firstFailure==DIAG_OK) a.diagnostic.firstFailure=f;
        a.diagnostic.latestFailure=f;
    }
    void markDiagnosticSuccess(CS35L41Amp&,DriverStage) {}
    void snapshotDiagnostics(CS35L41Amp&,const char*) {}
    void setProperty(const char* p,OSString* v) { properties[p]=v->value; }
    void setProperty(const char*,uint64_t,unsigned) {}
    void setProperty(const char*,bool) {}
    bool initCodec(CS35L41Amp&) { return false; }
    bool initializeHardwareErrata(CS35L41Amp&) { return false; }
    uint32_t calculateRegistersCRC32(CS35L41Amp&) { return 0; }
    void logPowerSnapshot(CS35L41Amp&) {}
    bool applyGPIO(CS35L41Amp&) { return false; }
    bool configureHardware(CS35L41Amp&) { return false; }
    void discoverFirmware(CS35L41Amp&) {}
    void fullDriverFlow() {
        assert(mPowerAvailable && mNeedsReinitialization);
        ++restores;
        for(auto& a:mAmps) if(a.present && !a.playbackFaulted) a.initialized=true;
    }
    void initializeFirmware(CS35L41Amp&,const char*) {}
    static const char* failureName(DiagnosticFailure) { return "failure"; }
    static const char* stageName(DriverStage) { return "stage"; }
'''

declarations = '\n'.join(function(name).split('{', 1)[0].replace('CirrusAudioFixup::', '') + ';' for name in names)

adapter = r'''
class FixupRegisterIOAdapter : public cirrus::core::RegisterIO {
    CirrusAudioFixup* mFixup;
    CS35L41Amp& mAmp;
public:
    FixupRegisterIOAdapter(CirrusAudioFixup* f, CS35L41Amp& a) : mFixup(f), mAmp(a) {}
    bool read(uint32_t reg, uint32_t* val) override { return mFixup->readRegister(mAmp, reg, val); }
    bool write(uint32_t reg, uint32_t val) override { return mFixup->writeRegister(mAmp, reg, val); }
    bool updateBits(uint32_t reg, uint32_t mask, uint32_t val) override { return mFixup->updateRegisterBits(mAmp, reg, mask, val); }
    bool pollBit(uint32_t r, uint32_t m, uint32_t e, uint32_t t) override { return mFixup->pollRegisterBit(mAmp, r, m, e, t); }
    bool bulkRead(uint32_t reg, uint8_t* buf, size_t len) override { return mFixup->bulkRead(mAmp, reg, buf, len); }
    bool bulkWrite(uint32_t, const uint8_t*, size_t) override { return true; }
};
'''

checks = r'''
int main() {
    for(unsigned fmt : {0x11U,0x21U,0x31U,0x931U}) assert(CirrusAudioFixup().supportedHdaFormat(fmt));
    for(unsigned fmt : {0U,0x30U,0x4031U,0x831U,0x8031U,0x41U,0xB1U,0x2031U}) assert(!CirrusAudioFixup().supportedHdaFormat(fmt));
    for(unsigned version : {0x1500U,0x1D00U}) {
        CirrusAudioFixup baseline;
        baseline.mAmps[0].firmwareIdVersion=version;
        baseline.runBackgroundMonitor();
        assert(baseline.mAmps[0].playbackActive && baseline.unmute==1);
        unsigned count=baseline.ops;
        for(unsigned i=1;i<=count;i++) {
            CirrusAudioFixup d;
            d.mAmps[0].firmwareIdVersion=version;
            d.failAt=i;
            d.runBackgroundMonitor();
            if(d.unmuteAfterFault) fprintf(stderr,"unmute after fault index=%u version=%X\n",i,version);
            assert(d.unmuteAfterFault==0);
            if(d.mAmps[0].playbackFaulted) assert(!d.mAmps[0].playbackActive);
        }
        printf("PASS playback version=0x%X single-I/O fault sweep (%u positions)\n",version,count);
    }
    for(unsigned mode=0;mode<4;mode++) {
        CirrusAudioFixup d;
        if(mode==0) d.pup=false;
        if(mode==1) d.pll=false;
        if(mode==2) d.mailbox=false;
        if(mode==3) { d.mAmps[0].monitorCount=0; d.mAmps[0].dspAlive=false; }
        d.runBackgroundMonitor();
        assert(!d.mAmps[0].playbackActive && d.mAmps[0].playbackFaulted && d.unmute==0);
    }
    CirrusAudioFixup shortStream;
    shortStream.endStreamBeforeUnmute=true;
    shortStream.runBackgroundMonitor();
    assert(shortStream.unmute==0 && !shortStream.mAmps[0].playbackActive && !shortStream.mAmps[0].playbackFaulted);
    shortStream.endStreamBeforeUnmute=false;
    shortStream.mHdaStreamActive=true; shortStream.streamEnded=false;
    shortStream.runBackgroundMonitor();
    assert(shortStream.mAmps[0].playbackActive && shortStream.unmute==1);
    CirrusAudioFixup badRoute;
    badRoute.corruptRoute=true;
    badRoute.runBackgroundMonitor();
    assert(badRoute.unmute==0 && badRoute.mAmps[0].playbackFaulted);
    CirrusAudioFixup noHda;
    noHda.mHdaControllerObserved=false;
    noHda.runBackgroundMonitor();
    assert(noHda.unmute==0);
    CirrusAudioFixup bypass;
    bypass.bypass=true;
    bypass.mAmps[0].monitorCount=0;
    bypass.mAmps[0].dspAlive=false;
    bypass.runBackgroundMonitor();
    assert(bypass.mAmps[0].playbackActive && (bypass.regs[CS35L41_AMP_DIG_VOL_CTRL]==0x8000 || bypass.regs[CS35L41_AMP_DIG_VOL_CTRL]==0x0000));
    assert(bypass.regs[CS35L41_AMP_GAIN_CTRL]==0x84);
    unsigned bypassOps=bypass.ops;
    for(unsigned i=1;i<=bypassOps;i++) {
        CirrusAudioFixup d;
        d.bypass=true; d.mAmps[0].monitorCount=0; d.mAmps[0].dspAlive=false;
        d.failAt=i; d.runBackgroundMonitor();
        assert(d.unmuteAfterFault==0);
    }
    CirrusAudioFixup persistent;
    persistent.runBackgroundMonitor();
    persistent.failAllFrom=persistent.ops+1;
    for(unsigned i=0;i<8;i++) persistent.runBackgroundMonitor();
    assert(persistent.mAmps[0].cleanupAttempts==3 && persistent.mAmps[0].playbackFaulted);
    unsigned stoppedOps=persistent.ops;
    persistent.runBackgroundMonitor();
    assert(persistent.ops==stoppedOps);
    puts("PASS bypass fault sweep, short-stream recovery, bad route, missing HDA, bounded persistent cleanup");
    CirrusAudioFixup baseline;
    baseline.runBackgroundMonitor();
    baseline.ops=0;
    assert(baseline.stopPlayback(baseline.mAmps[0]));
    unsigned count=baseline.ops;
    for(unsigned i=1;i<=count;i++) {
        CirrusAudioFixup d; d.runBackgroundMonitor(); d.ops=0; d.failAt=i;
        assert(!d.stopPlayback(d.mAmps[0]));
        assert(d.mAmps[0].playbackFaulted);
        assert(d.properties["Cirrus_Playback_Verdict_host"]!="SAFE_IDLE_VERIFIED");
    }
    for(unsigned mode=0;mode<2;mode++) {
        CirrusAudioFixup d; d.runBackgroundMonitor();
        if(mode==0) d.pdn=false; else d.mailbox=false;
        assert(!d.stopPlayback(d.mAmps[0]));
        assert(d.mAmps[0].playbackFaulted);
    }
    CirrusAudioFixup boot;
    assert(boot.bringupDSP(boot.mAmps[0]));
    assert(boot.verifyDSPAlive(boot.mAmps[0]));
    for(unsigned i=1;i<=boot.ops;i++) {
        CirrusAudioFixup d; d.failAt=i;
        bool started=d.bringupDSP(d.mAmps[0]);
        bool healthy=started && d.verifyDSPAlive(d.mAmps[0]);
        assert(!healthy);
        assert(d.unmute==0);
    }
    CirrusAudioFixup badBoot; badBoot.halo=false;
    assert(!badBoot.bringupDSP(badBoot.mAmps[0]));
    assert(!(badBoot.regs[CS35L41_DSP1_CCM_CORE_CTRL]&HALO_CORE_EN));
    Timer timer;
    CirrusAudioFixup power;
    power.mProbeTimer=&timer;
    power.runBackgroundMonitor();
    power.handlePowerChange(false);
    assert(!power.mPowerAvailable && !power.mAmps[0].playbackActive);
    assert(!power.mAmps[0].initialized && !power.mAmps[0].firmwareValidated && !power.mAmps[0].dspAlive);
    unsigned offOps=power.ops;
    power.runBackgroundMonitor(); power.handlePowerChange(false);
    assert(power.ops==offOps && timer.cancelled==1);
    power.handlePowerChange(true);
    assert(power.mPowerAvailable && !power.mNeedsReinitialization && power.restores==1);
    assert(power.ops==offOps);
    power.mStopping=true; power.handlePowerChange(false); power.runBackgroundMonitor();
    assert(power.ops==offOps);
    CirrusAudioFixup ro; ro.readonly=true;
    ro.handlePowerChange(false); ro.runBackgroundMonitor(); ro.handlePowerChange(true);
    assert(ro.ops==0 && !ro.mNeedsReinitialization);
    CirrusAudioFixup dirty; dirty.runBackgroundMonitor(); dirty.pdn=false;
    dirty.handlePowerChange(false); dirty.handlePowerChange(true);
    assert(dirty.mAmps[0].playbackFaulted && !dirty.mAmps[0].initialized && dirty.restores==1);
    Provider provider;
    CirrusAudioFixup transport; transport.mProvider=&provider;
    uint8_t byte=0;
    assert(transport.transferToAddress(0x40,&byte,1,nullptr,0));
    transport.mPowerAvailable=false;
    assert(!transport.transferToAddress(0x40,&byte,1,nullptr,0));
    transport.mPowerAvailable=true; transport.mStopping=true;
    assert(!transport.transferToAddress(0x40,&byte,1,nullptr,0));
    assert(provider.calls==1);
    assert(power.properties["Cirrus_Driver_Verdict"]!="READY_DSP");
    assert(dirty.properties["Cirrus_Driver_Verdict"]=="FAULT_LATCHED");
    CirrusAudioFixup verdict;
    verdict.mAmps[1]=verdict.mAmps[0];
    verdict.mAmps[0].dspAlive=false;
    verdict.publishDriverVerdict();
    assert(verdict.properties["Cirrus_Driver_Verdict"]=="OUTPUT_DISABLED_DSP_UNVERIFIED");
    verdict.bypass=true; verdict.publishDriverVerdict();
    assert(verdict.properties["Cirrus_Driver_Verdict"]=="READY_BYPASS_EXPLICIT");
    puts("PASS HDA PCM format validation, PM quiesce/synchronous restore, repeated sleep, readonly PM and latched cleanup fault");
    puts("PASS cleanup fault sweep, PUP/PLL/mailbox/PDN timeouts, firmware gate, boot/stop checks");
    unsigned newFailures=0;
    auto checkNew=[&](bool ok,const char* name) { if(!ok) { fprintf(stderr,"FAIL %s\n",name); ++newFailures; } };
    CirrusAudioFixup stereoShort;
    stereoShort.mAmps[1]=stereoShort.mAmps[0]; stereoShort.mAmps[1].name="right";
    stereoShort.endStreamBeforeUnmute=true;
    stereoShort.runBackgroundMonitor();
    checkNew(stereoShort.startsAfterStreamEnd==0 && stereoShort.unmute==0 &&
             !stereoShort.mAmps[1].playbackFaulted,"second amp cannot start from HDA state invalidated by first amp");
    CirrusAudioFixup clock;
    clock.failAt=4;
    checkNew(!clock.applyPLL(clock.mAmps[0]),"PLL configuration rejects status read failure");
    CirrusAudioFixup noClock;
    noClock.pll=false;
    checkNew(noClock.applyPLL(noClock.mAmps[0]),"PLL configuration allows idle BCLK absence");
    for(unsigned slot=0;slot<2;++slot) {
        CirrusAudioFixup d;
        d.mAmps[0].name=slot?"right":"left";
        d.mAmps[0].channel=slot?cirrus::core::AudioChannel::Right:cirrus::core::AudioChannel::Left;
        d.regs[CS35L41_SP_FRAME_RX_SLOT]=0xABCDEF00;
        checkNew(d.applyASP(d.mAmps[0]),"ASP channel setup");
        checkNew(d.regs[CS35L41_SP_FRAME_RX_SLOT]==(0xABCDEF00U|slot),"ASP slot selection preserves other fields");
    }
    CirrusAudioFixup external;
    external.regs[CS35L41_PWR_CTRL2_REG]=0x30;
    checkNew(external.powerUpAmplifier(external.mAmps[0]) && !(external.regs[CS35L41_PWR_CTRL2_REG]&0x30),"external boost init disables internal boost");
    external.regs[CS35L41_SP_FORMAT]=0x20200200;
    external.regs[CS35L41_PWR_CTRL2_REG]|=0x20;
    checkNew(!external.verifyIdleConfiguration(external.mAmps[0]),"idle verification rejects internal boost enabled");
    for(uint32_t bit : {0x80000000U,0x100U,0x8000U,0x20000U,0x40U,0x80U}) {
        CirrusAudioFixup initFault;
        initFault.regs[0x10010]=bit;
        checkNew(!initFault.powerUpAmplifier(initFault.mAmps[0]),"idle init rejects protection fault");
        bool released=false;
        for(auto w:initFault.writes) if(w.first==0x2034) released=true;
        checkNew(!released,"idle init cannot release a protection fault to become ready");
        CirrusAudioFixup startFault;
        startFault.regs[0x10010]=bit;
        startFault.runBackgroundMonitor();
        checkNew(startFault.unmute==0 && startFault.mAmps[0].playbackFaulted,"latched protection fault blocks start");
        CirrusAudioFixup activeFault;
        activeFault.runBackgroundMonitor(); activeFault.regs[0x10010]|=bit;
        activeFault.runBackgroundMonitor();
        checkNew(activeFault.mAmps[0].playbackFaulted && !activeFault.mAmps[0].playbackActive,"protection fault stops active playback");
        CirrusAudioFixup prepareFault;
        prepareFault.injectProtectionAtPup=bit; prepareFault.runBackgroundMonitor();
        checkNew(prepareFault.unmute==0 && prepareFault.mAmps[0].playbackFaulted,"fault arriving during prepare blocks unmute");
    }
    for(unsigned fault=0;fault<6;++fault) {
        CirrusAudioFixup d; d.runBackgroundMonitor();
        if(fault==0) d.pll=false;
        if(fault==1) d.regs[0x13004]=0xFFFFFF;
        if(fault==2) d.halo=false;
        if(fault==3) d.regs[CS35L41_DSP1_CCM_CORE_CTRL]=0;
        if(fault==4) d.regs[CS35L41_PWR_CTRL1_REG]=0;
        if(fault==5) d.regs[CS35L41_PWR_CTRL2_REG]|=0x20;
        d.runBackgroundMonitor();
        checkNew(d.mAmps[0].playbackFaulted && !d.mAmps[0].playbackActive,"lost active hardware/DSP state stops playback");
    }
    CirrusAudioFixup ending;
    CirrusAudioFixup clockLostDuringPrepare;
    clockLostDuringPrepare.losePllAtPup=true;
    clockLostDuringPrepare.runBackgroundMonitor();
    checkNew(clockLostDuringPrepare.unmute==0 && clockLostDuringPrepare.mAmps[0].playbackFaulted,
             "PLL loss during prepare blocks unmute");
    ending.runBackgroundMonitor(); ending.pll=false; ending.mHdaStreamActive=false;
    ending.runBackgroundMonitor(); ending.runBackgroundMonitor();
    checkNew(!ending.mAmps[0].playbackFaulted && !ending.mAmps[0].playbackActive,"normal HDA close with clocks stopped stays recoverable");
    CirrusAudioFixup stable;
    stable.runBackgroundMonitor(); stable.ops=0; stable.runBackgroundMonitor();
    unsigned activeOps=stable.ops;
    checkNew(stable.mAmps[0].playbackActive && !stable.mAmps[0].playbackFaulted && stable.unmute==1,"healthy active monitor does not repeat start");
    for(unsigned i=1;i<=activeOps;++i) {
        CirrusAudioFixup d; d.runBackgroundMonitor(); d.ops=0; d.failAt=i;
        d.runBackgroundMonitor();
        checkNew(d.mAmps[0].playbackFaulted && !d.mAmps[0].playbackActive,"active monitor I/O fault causes cleanup");
    }
    CirrusAudioFixup stuck;
    stuck.stuckBoost=true; stuck.runBackgroundMonitor();
    checkNew(stuck.unmute==0 && stuck.mAmps[0].playbackFaulted &&
             stuck.properties["Cirrus_Playback_Verdict_host"]=="CLEANUP_UNVERIFIED","uncleared boost bit blocks unmute and safe-idle verdict");
    CirrusAudioFixup headphoneCycle;
    headphoneCycle.runBackgroundMonitor();
    assert(headphoneCycle.mAmps[0].playbackActive);
    headphoneCycle.pll = false;
    headphoneCycle.pup = false;
    headphoneCycle.runBackgroundMonitor();
    checkNew(headphoneCycle.mAmps[0].playbackFaulted && !headphoneCycle.mAmps[0].playbackActive, "headphone plug stops playback");
    for(int tick = 0; tick < 250; ++tick) headphoneCycle.runBackgroundMonitor();
    checkNew(headphoneCycle.unmute == 1 && !headphoneCycle.mAmps[0].playbackActive &&
             headphoneCycle.mAmps[0].pllRecoveryPending && headphoneCycle.mAmps[0].pllRecoveryAttempts == 0,
             "headphone clock absence stays muted without exhausting recovery");
    headphoneCycle.pup = true;
    headphoneCycle.pll = true;
    for(int tick = 0; tick < 25; ++tick) headphoneCycle.runBackgroundMonitor();
    checkNew(!headphoneCycle.mAmps[0].playbackFaulted && headphoneCycle.mAmps[0].playbackActive, "headphone unplug restores speaker playback");
    CirrusAudioFixup recoveryPowerFault;
    recoveryPowerFault.runBackgroundMonitor();
    recoveryPowerFault.pll = false;
    recoveryPowerFault.runBackgroundMonitor();
    recoveryPowerFault.pll = true;
    recoveryPowerFault.pup = false;
    for(int tick = 0; tick < 30; ++tick) recoveryPowerFault.runBackgroundMonitor();
    checkNew(recoveryPowerFault.mAmps[0].playbackFaulted &&
             recoveryPowerFault.mAmps[0].diagnostic.latestFailure == DIAG_POWER_UP_TIMEOUT &&
             recoveryPowerFault.unmute == 1, "power timeout with clock present remains latched");
    CirrusAudioFixup recoveryProtectionFault;
    recoveryProtectionFault.runBackgroundMonitor();
    recoveryProtectionFault.pll = false;
    recoveryProtectionFault.pup = false;
    recoveryProtectionFault.runBackgroundMonitor();
    for(int tick = 0; tick < 10; ++tick) recoveryProtectionFault.runBackgroundMonitor();
    recoveryProtectionFault.regs[0x10010] |= 0x80000000;
    for(int tick = 0; tick < 30; ++tick) recoveryProtectionFault.runBackgroundMonitor();
    recoveryProtectionFault.pll = true;
    recoveryProtectionFault.pup = true;
    for(int tick = 0; tick < 30; ++tick) recoveryProtectionFault.runBackgroundMonitor();
    checkNew(recoveryProtectionFault.mAmps[0].playbackFaulted && recoveryProtectionFault.unmute == 1,
             "protection fault during clock wait prevents recovery");
    if(newFailures) return 1;
    CirrusAudioFixup latchedIo;
    latchedIo.runBackgroundMonitor(); latchedIo.failAt=latchedIo.ops+1; latchedIo.runBackgroundMonitor();
    latchedIo.failAt=0;
    unsigned afterFault=latchedIo.ops;
    for(unsigned i=0;i<20;++i) latchedIo.runBackgroundMonitor();
    assert(latchedIo.mAmps[0].playbackFaulted && latchedIo.ops==afterFault && latchedIo.unmute==1);
    CirrusAudioFixup boundedPll;
    boundedPll.runBackgroundMonitor(); boundedPll.pll=false; boundedPll.runBackgroundMonitor();
    for(unsigned i=0;i<30;++i) boundedPll.runBackgroundMonitor();
    assert(boundedPll.mAmps[0].playbackFaulted && boundedPll.mAmps[0].pllRecoveryAttempts==3);
    CirrusAudioFixup halted; halted.mDebugPhaseHalted=true; halted.runBackgroundMonitor();
    assert(halted.ops==0 && halted.unmute==0);
    puts("PASS configuration, external boost invariants, protection and active-state loss checks");
}
'''

with tempfile.TemporaryDirectory(prefix='cirrus-runtime-check-') as directory:
    tmp = Path(directory)
    source = tmp / 'runtime.cpp'
    source.write_text(preamble + LEGACY_CONSTANTS + mock + '\n' + declarations + '\n};\n' + adapter + functions + checks, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O0', '-I' + str(ROOT / 'CirrusAudioFixup'), str(source), '-o', str(tmp / 'runtime.exe')], check=True)
    subprocess.run([str(tmp / 'runtime.exe')], check=True)
