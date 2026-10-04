from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')
REGISTERS = (ROOT / 'CirrusAudioFixup/Devices/CS35L41/Hardware/Registers.hpp').read_text(encoding='utf-8')

def function(name):
    start = re.search(r'(?:bool|void) CirrusAudioFixup::' + name + r'\(', SOURCE).start()
    end = SOURCE.index('\n}', start) + 2
    return SOURCE[start:end]

names = ['checkProtectionStatus', 'stopPlayback', 'runBackgroundMonitor', 'sendMailboxCommand', 'unlockTestKey', 'lockTestKey', 'stopDSP', 'bringupDSP', 'verifyDSPAlive', 'handlePowerChange', 'supportedHdaFormat', 'transferToAddress', 'publishDriverVerdict', 'applyPLL', 'applyASP', 'applyRegisterSequence', 'powerUpAmplifier', 'verifyIdleConfiguration']
functions = '\n'.join(function(name) for name in names)
sequences = '\n'.join(re.search(r'static const RegisterSequence\s+'+name+r'\[\]\s*=\s*\{.*?\};', SOURCE, re.S).group(0)
                      for name in ['pll_sequence', 'asp_sequence'])
constants = '\n'.join(line for line in (HEADER+'\n'+REGISTERS).splitlines() if re.match(r'#define (?:CS35L41_|HALO_|CSPL_)\w+\s+(?:0x|[0-9])',line))
states_start = HEADER.find('enum class TraceSource') if 'enum class TraceSource' in HEADER else HEADER.index('enum TraceSource')
states = HEADER[states_start:HEADER.index('struct TraceEntry')]
amp = HEADER[HEADER.index('struct CS35L41Amp {'):HEADER.index('struct FirmwareImage;')]

preamble = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <map>
#include <vector>
#include <string>
using UInt8=uint8_t;
using UInt16=uint16_t;
using UInt32=uint32_t;
using IOReturn=int;
struct RegisterSequence { UInt32 reg,mask,value,delay_us; bool updateBits; };
constexpr int kIOReturnSuccess=0;
constexpr int kIOReturnNotReady=-1;
constexpr int kIOReturnBadArgument=-2;
constexpr bool kOSBooleanTrue=true;
#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
inline void IODelay(unsigned) {}
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
'''
mock = r'''
class CirrusAudioFixup {
public:
    CS35L41Amp mAmps[2]{};
    Timer* mProbeTimer=nullptr;
    Provider* mProvider=nullptr;
    bool mPowerAvailable=true,mStopping=false,mNeedsReinitialization=false;
    bool mHdaConverterPrepared=true,mHdaTopologyLogged=true,readonly=false;
    bool mHdaControllerObserved=true,mHdaStreamActive=true;
    unsigned mHdaLastStreamTag=1,mHdaLastFormat=0,mHdaLastDescriptor=0;
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
        if(r==CS35L41_AMP_DIG_VOL_CTRL && v==0x8000) { ++unmute; if(failed) ++unmuteAfterFault; }
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
    bool updateRegisterBits(CS35L41Amp& a,uint32_t r,uint32_t mask,uint32_t v,TraceSource s=TRACE_OTHER) {
        uint32_t old=0;
        return readRegister(a,r,&old,s) && writeRegister(a,r,(old&~mask)|(v&mask),s);
    }
    bool bulkRead(CS35L41Amp&,uint32_t,uint8_t* p,unsigned n,TraceSource=TRACE_OTHER) {
        if(!step()) return false; memset(p,0,n); return true;
    }
    bool syncAlc287HdaCodec() {
        if(endStreamBeforeUnmute && ++syncCalls>1) { streamEnded=true; mHdaStreamActive=false; }
        return mHdaStreamActive;
    }
    bool bootArgEnabled(const char* name) { return strcmp(name,"cirrus_readonly")==0 ? readonly : strcmp(name,"cirrus_nodsp")==0 ? bypass : false; }
    void setDiagnosticStage(CS35L41Amp&,DriverStage) {}
    template<class... T> void recordDiagnosticFailure(CS35L41Amp&,T...) {}
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
    assert(bypass.mAmps[0].playbackActive && bypass.regs[CS35L41_AMP_DIG_VOL_CTRL]==0x8000);
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
    assert(power.ops==offOps); // Mock restoration is synchronous; no playback is started here.
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
    clock.failAt=4; // Three clock writes succeeded; status read fails.
    checkNew(!clock.applyPLL(clock.mAmps[0]),"PLL configuration rejects status read failure");
    CirrusAudioFixup noClock;
    noClock.pll=false;
    checkNew(noClock.applyPLL(noClock.mAmps[0]),"PLL configuration allows idle BCLK absence");
    for(unsigned slot=0;slot<2;++slot) {
        CirrusAudioFixup d;
        d.mAmps[0].name=slot?"right":"left";
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
    printf("PASS active monitor single-I/O fault sweep (%u positions)\n",activeOps);
    if(newFailures) return 1;
    puts("PASS configuration, external boost invariants, protection and active-state loss checks");
}
'''
declarations = '\n'.join(function(name).split('{',1)[0].replace('CirrusAudioFixup::','')+';' for name in names)
with tempfile.TemporaryDirectory(prefix='cirrus-runtime-check-') as directory:
    tmp=Path(directory)
    source=tmp/'runtime.cpp'
    source.write_text(preamble+constants+'\n'+sequences+'\n'+states+amp+mock+declarations+'\n};\n'+functions+checks,encoding='utf-8')
    subprocess.run(['g++','-std=c++17','-O0',str(source),'-o',str(tmp/'runtime.exe')],check=True)
    subprocess.run([str(tmp/'runtime.exe')],check=True)
