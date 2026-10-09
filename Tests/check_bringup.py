from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')

def function(name):
    start = re.search(r'(?:bool|void) CirrusAudioFixup::' + name + r'\(', SOURCE).start()
    return SOURCE[start:SOURCE.index('\n}', start) + 2]

names = ['fullDriverFlow', 'initializeCS35L41', 'initCodec', 'handlePowerChange', 'publishDriverVerdict', 'stopAfterDebugStage']
amp = HEADER[HEADER.index('struct AmplifierState {'):HEADER.index('struct FirmwareImage;')]

preamble = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Devices/CS35L41/CS35L41Device.hpp"
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t;
using IOReturn=int;
struct Timer { void cancelTimeout() {} void setTimeoutMS(unsigned) {} };
struct OSString {
    std::string value;
    static OSString* withCString(const char* s) { return new OSString{s}; }
    void release() { delete this; }
};
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
    bool mPowerAvailable=true,mStopping=false,mNeedsReinitialization=false;
    bool mDebugPhaseHalted=false;
    void clearHdaCache() {}
    std::string phase;
    bool bootArgStrEquals(const char*,const char* s) { return phase==s; }
    bool stopAfterDebugStage(const char*);
    cirrus::platform::hda::HDAStreamState mHdaState;
    bool& mHdaConverterPrepared = mHdaState.converterPrepared;
    bool& mHdaControllerObserved = mHdaState.observed;
    bool& mHdaStreamActive = mHdaState.streamActive;
    bool& mHdaTopologyLogged = mHdaState.topologyLogged;
    uint8_t& mHdaLastDescriptor = mHdaState.lastDescriptor;
    uint8_t& mHdaLastStreamTag = mHdaState.lastStreamTag;
    uint16_t& mHdaLastFormat = mHdaState.lastFormat;
    uint32_t& mHdaMissCount = mHdaState.missCount;

    int mLastTransferReturn=-1;
    bool bypass=false,readonly=false,otpError=false,otpReadError=false,identityReadError=false;
    bool cleanupError=false,haltError=false;
    unsigned io=0,failIO=0;
    std::string failStage;
    unsigned discoveries=0,uploads=0,stops[2]{},halts[2]{},resets[2]{};
    bool muted[2]{},powered[2]{};
    std::map<std::string,std::string> properties;
    CirrusAudioFixup() {
        mAmps[0].name="L"; mAmps[0].address=0;
        mAmps[1].name="R"; mAmps[1].address=1;
        for(auto& a:mAmps) a.model=cirrus::core::CodecModel::CS35L41;
        mAmps[1].channel=cirrus::core::AudioChannel::Right;
    }
    static const char* failureName(DiagnosticFailure) { return "failure"; }
    static const char* stageName(DriverStage) { return "stage"; }

    unsigned faultAddress=0;
    bool fail(CS35L41Amp& a,const char* stage) { return a.address==faultAddress && failStage==stage; }
    bool transfer(CS35L41Amp& a) {
        if(++io!=failIO) return true;
        recordDiagnosticFailure(a,DIAG_I2C_TRANSFER);
        return false;
    }
    bool readRegister(CS35L41Amp& a,uint32_t r,uint32_t* v,TraceSource=TRACE_OTHER) {
        if(!transfer(a)) return false;
        if(a.address==0 && ((identityReadError && r==registers::kRegDeviceId) ||
                            (otpReadError && r==registers::kRegIrq1Status3))) return false;
        *v = r==registers::kRegDeviceId ? registers::kValDeviceId : r==registers::kRegRevisionId ? 0xB2 :
             r==registers::kRegIrq1Status3 && otpError && a.address==0 ? 0x80000000U : 0;
        return true;
    }
    bool writeRegister(CS35L41Amp& a,uint32_t r,uint32_t,TraceSource=TRACE_OTHER) {
        if(!transfer(a)) return false;
        if(r==registers::kRegSoftwareReset) ++resets[a.address];
        return true;
    }
    bool pollRegisterBit(CS35L41Amp& a,uint32_t,uint32_t,uint32_t,uint32_t,TraceSource=TRACE_OTHER) {
        return !fail(a,"otp-timeout");
    }
    uint32_t calculateRegistersCRC32(CS35L41Amp&) { return 0; }
    bool bootArgEnabled(const char* s) {
        return (strcmp(s,"-cirrusnodsp")==0) ? bypass :
               (strcmp(s,"-cirrusro")==0) ? readonly : false;
    }
    void setProperty(const char* p,OSString* s) { properties[p]=s->value; }
    void setProperty(const char* p,const char* s) { properties[p]=s; }
    void setProperty(const char*,uint64_t,unsigned) {}
    void setDiagnosticStage(CS35L41Amp& a,DriverStage s) { a.diagnostic.stage=s; }
    void markDiagnosticSuccess(CS35L41Amp& a,DriverStage s) { a.diagnostic.lastGoodStage=s; }
    template<class... T> void recordDiagnosticFailure(CS35L41Amp& a,DiagnosticFailure f,T...) {
        if(a.diagnostic.firstFailure==DIAG_OK) a.diagnostic.firstFailure=f;
        a.diagnostic.latestFailure=f; ++a.diagnostic.failureCount;
    }
    bool initializeHardwareErrata(CS35L41Amp& a) { return !fail(a,"errata"); }
    bool applyPLL(CS35L41Amp& a) { return !fail(a,"pll"); }
    bool applyASP(CS35L41Amp& a) { return !fail(a,"asp"); }
    bool applyGPIO(CS35L41Amp& a) { return !fail(a,"gpio"); }
    bool configureHardware(CS35L41Amp& a) { return !fail(a,"platform"); }
    void discoverFirmware(CS35L41Amp& a) {
        ++discoveries;
        static uint8_t blob=1;
        a.wmfwData=a.binData=fail(a,"missing-firmware") ? nullptr : &blob;
        a.wmfwSize=a.binSize=1;
    }
    void initializeFirmware(CS35L41Amp& a,const char*) {
        ++uploads;
        a.firmwareValidated=a.dspAlive=!fail(a,"upload");
        a.monitorCount=a.dspAlive?1:0;
        a.haloStateRegister=0x02800398; a.haloHeartbeatRegister=0x0280039C;
        if(a.firmwareValidated && phase=="firmware") {
            a.dspAlive=false; a.monitorCount=0; stopAfterDebugStage("firmware");
        }
    }
    bool powerUpAmplifier(CS35L41Amp& a) {
        bool ok=!fail(a,"idle-prepare");
        muted[a.address]=ok; powered[a.address]=!ok;
        return ok;
    }
    bool verifyIdleConfiguration(CS35L41Amp& a) {
        if(fail(a,"idle-verify")) { powered[a.address]=true; muted[a.address]=false; return false; }
        return true;
    }
    bool verifyDSPAlive(CS35L41Amp& a) { return !fail(a,"dsp-verify"); }
    void logASPSnapshot(CS35L41Amp&) {}
    bool stopPlayback(CS35L41Amp& a) {
        ++stops[a.address]; muted[a.address]=!cleanupError; powered[a.address]=cleanupError;
        a.playbackActive=cleanupError;
        return !cleanupError;
    }
    bool stopDSP(CS35L41Amp& a) {
        ++halts[a.address]; a.dspAlive=false; a.monitorCount=0;
        return !haltError;
    }
    void publishDriverVerdict();
    void handlePowerChange(bool powered);
    bool initCodec(CS35L41Amp&);
    void fullDriverFlow();
    void initializeCS35L41(AmplifierState&);
};

class FixupRegisterIOAdapter : public cirrus::core::RegisterIO {
    CirrusAudioFixup* mFixup;
    CS35L41Amp& mAmp;
public:
    FixupRegisterIOAdapter(CirrusAudioFixup* f, CS35L41Amp& a) : mFixup(f), mAmp(a) {}
    bool read(uint32_t reg, uint32_t* val) override { return mFixup->readRegister(mAmp, reg, val); }
    bool write(uint32_t reg, uint32_t val) override { return mFixup->writeRegister(mAmp, reg, val); }
    bool updateBits(uint32_t, uint32_t, uint32_t) override { return true; }
    bool pollBit(uint32_t r, uint32_t m, uint32_t e, uint32_t t) override { return mFixup->pollRegisterBit(mAmp, r, m, e, t); }
    bool bulkRead(uint32_t, uint8_t*, size_t) override { return true; }
    bool bulkWrite(uint32_t, const uint8_t*, size_t) override { return true; }
};
'''

checks = r'''
unsigned failures=0;
void check(bool ok,const std::string& label) { if(!ok) { ++failures; printf("FAIL %s\n",label.c_str()); } }
int main() {
    for(const char* stage : {"probe","otp","errata","clock","asp","gpio","platform","firmware","dsp"}) {
        CirrusAudioFixup d; d.phase=stage; d.fullDriverFlow();
        check(d.mDebugPhaseHalted && !d.mAmps[0].initialized && !d.mAmps[1].initialized,"phase never publishes readiness");
        check(d.properties["Cirrus_Driver_Verdict"]=="DEBUG_PHASE_HALTED","phase verdict");
        check(d.uploads==((std::string(stage)=="dsp" || std::string(stage)=="firmware")?2U:0U),"phase cannot upload past requested stage");
        if(std::string(stage)=="probe") check(d.io==0,"probe phase never resets silicon");
    }
    CirrusAudioFixup phaseFailure; phaseFailure.phase="firmware";
    phaseFailure.failStage="upload"; phaseFailure.faultAddress=1;
    phaseFailure.fullDriverFlow();
    check(phaseFailure.mDebugPhaseHalted && phaseFailure.mAmps[1].playbackFaulted,
          "first amp debug halt cannot conceal second amp upload failure");
    check(phaseFailure.stops[1]>0 && phaseFailure.halts[1]>0,
          "failed second amp still receives synchronous cleanup");
    check(phaseFailure.properties["Cirrus_Driver_Verdict"]=="FAULT_LATCHED",
          "fault takes precedence over debug phase verdict");
    CirrusAudioFixup ro; ro.readonly=true; ro.fullDriverFlow();
    check(ro.io==0 && ro.uploads==0,"readonly flow never writes hardware");
    CirrusAudioFixup unknown; unknown.mAmps[0].model=cirrus::core::CodecModel::CS35L56; unknown.fullDriverFlow();
    check(unknown.resets[0]==0 && unknown.mAmps[0].playbackFaulted,"unsupported backend rejected before reset");
    CirrusAudioFixup codec;
    check(codec.initCodec(codec.mAmps[0]),"normal OTP boot");
    for(unsigned i=1;i<=codec.io;++i) {
        CirrusAudioFixup d; d.failIO=i;
        check(!d.initCodec(d.mAmps[0]),"codec init rejects I/O fault "+std::to_string(i));
        check(d.mAmps[0].diagnostic.firstFailure==DIAG_I2C_TRANSFER,"codec init preserves transport first failure");
    }
    CirrusAudioFixup otp;
    otp.otpError=true;
    check(!otp.initCodec(otp.mAmps[0]),"OTP_BOOT_DONE plus OTP_BOOT_ERR must reject boot");
    check(otp.mAmps[0].diagnostic.firstFailure!=DIAG_OK,"OTP error has an explicit diagnostic");
    check(otp.mAmps[0].diagnostic.firstFailure==DIAG_OTP_BOOT_ERROR,"OTP error is distinct from timeout");
    CirrusAudioFixup unreadable;
    unreadable.otpReadError=true;
    check(!unreadable.initCodec(unreadable.mAmps[0]),"OTP status unreadable must reject boot");
    CirrusAudioFixup absent;
    absent.mAmps[0].present=true; absent.identityReadError=true;
    check(!absent.initCodec(absent.mAmps[0]) && !absent.mAmps[0].present,"failed identity read clears stale presence");
    for(const char* stage : {"otp-timeout","errata","pll","asp","gpio","platform","missing-firmware","upload","idle-prepare","idle-verify","dsp-verify"}) {
        CirrusAudioFixup d;
        d.failStage=stage;
        auto& a=d.mAmps[0];
        a.initialized=a.firmwareValidated=a.dspAlive=true; a.monitorCount=1;
        a.haloStateRegister=0xDEAD; a.diagnosticControlCount=1;
        d.fullDriverFlow();
        std::string prefix=std::string(stage)+": ";
        check(!a.initialized && !a.firmwareValidated && !a.dspAlive && a.monitorCount==0,prefix+"invalidate readiness");
        check(a.playbackFaulted,prefix+"latch failure before returning");
        check(d.stops[0]>0 && d.halts[0]>0 && d.muted[0] && !d.powered[0],prefix+"synchronous output/core cleanup");
        check(a.haloStateRegister==0 && a.haloHeartbeatRegister==0 && a.diagnosticControlCount==0,prefix+"invalidate control addresses");
        check(a.diagnostic.firstFailure!=DIAG_OK,prefix+"preserve first failure");
        check(d.mAmps[1].initialized && d.mAmps[1].firmwareValidated,prefix+"other amp completes independently");
        unsigned resets=d.resets[0]; d.fullDriverFlow();
        check(d.resets[0]==resets,prefix+"latched failure never auto retries reset");
    }
    CirrusAudioFixup bypass;
    bypass.bypass=true; bypass.fullDriverFlow();
    check(bypass.discoveries==0 && bypass.uploads==0,"explicit bypass skips firmware and coefficient loading");
    for(auto& a:bypass.mAmps) {
        check(a.initialized && !a.dspAlive && !a.firmwareValidated && a.monitorCount==0,"explicit bypass leaves DSP halted");
        check(bypass.halts[a.address]>0,"explicit bypass verifies core stop");
    }
    check(bypass.properties["Cirrus_Driver_Verdict"]=="READY_BYPASS_EXPLICIT","explicit bypass verdict");
    CirrusAudioFixup badBypass;
    badBypass.bypass=true; badBypass.haltError=true; badBypass.fullDriverFlow();
    check(!badBypass.mAmps[0].initialized && badBypass.mAmps[0].playbackFaulted && badBypass.uploads==0,
          "bypass cannot become ready if core halt fails");
    for(unsigned mode=0;mode<2;++mode) {
        CirrusAudioFixup d; d.failStage="idle-verify";
        d.cleanupError=mode==0; d.haltError=mode==1;
        d.fullDriverFlow();
        auto& a=d.mAmps[0];
        check(a.diagnostic.firstFailure==DIAG_IDLE_INVARIANT,"rollback failure preserves original idle invariant");
        check(a.playbackFaulted && !a.initialized && !a.firmwareValidated,"failed rollback cannot publish readiness");
        check(d.properties["Cirrus_Driver_Verdict"]=="FAULT_LATCHED","failed rollback verdict");
        check(d.halts[0]>0,"core halt still attempted after output cleanup fails");
    }
    CirrusAudioFixup normal; normal.fullDriverFlow();
    check(normal.uploads==2 && normal.properties["Cirrus_Driver_Verdict"]=="READY_DSP","normal DSP startup");
    normal.handlePowerChange(false); normal.handlePowerChange(true);
    check(normal.uploads==4 && normal.mAmps[0].initialized && normal.mAmps[1].initialized,"wake executes actual boot orchestration");
    CirrusAudioFixup wake; wake.fullDriverFlow(); wake.handlePowerChange(false);
    wake.failStage="dsp-verify"; wake.handlePowerChange(true);
    check(!wake.mAmps[0].initialized && wake.mAmps[0].playbackFaulted && wake.muted[0],"wake verify failure cannot reuse pre-sleep readiness");
    if(failures) { printf("FAIL bringup assertions=%u\n",failures); return 1; }
    printf("PASS codec init single-I/O fault sweep (%u positions)\n",codec.io);
    puts("PASS OTP error/read failure, stale identity, 11 boot-stage faults, explicit bypass, cleanup faults and actual boot/wake orchestration");
}
'''

with tempfile.TemporaryDirectory(prefix='cirrus-bringup-check-') as directory:
    tmp = Path(directory)
    source = tmp / 'bringup.cpp'
    source.write_text(preamble + mock + '\n'.join(function(name) for name in names) + checks, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O0', '-I' + str(ROOT / 'CirrusAudioFixup'), str(source), '-o', str(tmp / 'bringup.exe')], check=True)
    subprocess.run([str(tmp / 'bringup.exe')], check=True)
