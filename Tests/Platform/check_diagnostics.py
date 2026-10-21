import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from production_source_fixture import service_source
from host_build import sanitizer_flags
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = service_source()
header = (ROOT / 'CirrusAudioFixup/Diagnostics/DiagnosticTypes.hpp').read_text(encoding='utf-8')
start = source.index('void CirrusAudioFixup::recordDiagnosticFailure(')
function = source[start:source.index('\n}', start) + 2]
states = header[header.index('enum class DriverStage'):header.index('struct TraceEntry')]

preamble = r'''
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
using UInt32=uint32_t;
using IOReturn=int;
constexpr int kIOReturnSuccess=0;
bool gCirrusDebug=true;
#define CIRRUS_ERR(...) (++logs)
struct OSString {
    std::string value;
    static OSString* withCString(const char* s) { return new OSString{s}; }
    void release() { delete this; }
};
namespace cirrus { namespace diagnostics {
'''

mock = r'''
}}
using namespace cirrus::diagnostics;
struct CS35L41Amp { const char* name="left"; DiagnosticState diagnostic; };
using AmplifierState = CS35L41Amp;
class CirrusAudioFixup {
public:
    unsigned logs=0,publications=0,snapshots=0,dumps=0;
    bool mCapturingFailureSnapshot=false;
    std::string captureOrder;
    std::map<std::string,uint64_t> numbers;
    void setProperty(const char* p,uint64_t v,unsigned) { numbers[p]=v; }
    void setProperty(const char*,OSString*) {}
    void setProperty(const char*,const char*) {}
    const char* failureName(DiagnosticFailure) { return "failure"; }
    const char* stageName(DriverStage) { return "stage"; }
    void publishStatistics() { ++publications; }
    void captureFailureSnapshot(CS35L41Amp&,DiagnosticFailure) { ++snapshots;captureOrder+="S"; }
    void dumpTraceBuffer(const char*,const char*) { ++dumps;captureOrder+="T"; }
    void recordDiagnosticFailure(AmplifierState&,DiagnosticFailure,UInt32,UInt32,UInt32,IOReturn,bool);
};
'''

checks = r'''
int main() {
    unsigned failures=0;
    auto check=[&](bool ok,const char* label) { if(!ok) { ++failures; printf("FAIL %s\n",label); } };
    CirrusAudioFixup d; CS35L41Amp amp;
    auto record=[&](UInt32 expected,IOReturn ret) {
        d.recordDiagnosticFailure(amp,DIAG_I2C_TRANSFER,0x2014,expected,0,ret,true);
    };
    amp.diagnostic.stage=STAGE_NONE;
    record(4,-77);
    check(d.captureOrder=="TS","trace freezes before diagnostic hardware reads");
    record(4,-88);
    check(d.numbers["Cirrus_Diag_IOReturn_left"]==uint32_t(-88) && d.publications==2 && d.logs==2,
          "IOReturn-only change published and logged immediately");
    record(8,-88);
    check(d.numbers["Cirrus_Diag_Expected_left"]==8 && d.publications==3 && d.logs==3,
          "expected-only change published and logged immediately");
    check(d.snapshots==1 && d.dumps==1,"detail changes do not repeat same-failure snapshot");
    unsigned published=d.publications,logged=d.logs;
    while(amp.diagnostic.failureCount<31) record(8,-88);
    check(d.publications==published && d.logs==logged,"identical failures remain rate limited");
    record(8,-88);
    check(d.publications==published+1 && d.logs==logged && d.numbers["Cirrus_Diag_FailureCount_left"]==32,
          "periodic publication retained at failure 32");
    d.recordDiagnosticFailure(amp,DIAG_DSP_MAILBOX,0x13004,2,1,0,true);
    check(amp.diagnostic.firstFailure==DIAG_I2C_TRANSFER && amp.diagnostic.latestFailure==DIAG_DSP_MAILBOX,
          "first failure preserved when latest changes");
    check(d.snapshots==2 && d.dumps==2,"new failure class captures snapshot");
    check(amp.diagnostic.firstReg==0x2014 && amp.diagnostic.firstExpected==4 && amp.diagnostic.firstIOReturn==-77,
          "original transfer evidence survives cleanup failures");
    check(d.numbers["Cirrus_Diag_FirstIOReturn_left"]==uint32_t(-77),"first native return property preserved");
    CirrusAudioFixup transfer;CS35L41Amp transferAmp;
    transfer.recordDiagnosticFailure(transferAmp,DIAG_I2C_TRANSFER,0x11,1,0,-55,false);
    check(transfer.dumps==1 && transfer.snapshots==0,"first transfer freezes history without invasive snapshot reads");
    transfer.recordDiagnosticFailure(transferAmp,DIAG_I2C_TRANSFER,0x11,1,0,-55,true);
    check(transfer.dumps==2 && transfer.snapshots==1,"outer failure can capture snapshot without replacing first evidence");
    gCirrusDebug=false;auto snapshots=d.snapshots;auto dumps=d.dumps;
    d.recordDiagnosticFailure(amp,DIAG_DSP_BOOT,0x99,3,4,-9,true);
    check(d.snapshots==snapshots && d.dumps==dumps,"quiet Release failure performs no snapshot reads or trace serialization");
    amp.diagnostic.failureCount=UINT32_MAX;
    d.recordDiagnosticFailure(amp,DIAG_DSP_BOOT,0x99,3,4,-9,true);
    check(amp.diagnostic.failureCount==UINT32_MAX,"failure counter saturates");
    printf("%s diagnostic change publication, rate limiting and snapshot deduplication\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
'''

with tempfile.TemporaryDirectory(prefix='cirrus-diagnostics-') as directory:
    tmp = Path(directory)
    cpp = tmp / 'check.cpp'
    cpp.write_text(preamble + states + mock + function + checks, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O0', str(cpp), '-o', str(tmp / 'check.exe')], check=True)
    subprocess.run([str(tmp / 'check.exe')], check=True)

# Execute the production circular recorder/dumper. Allocations and publication
# assert that no recorder lock is held, including allocation failure paths.
def extract(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]
trace_mock=r'''
#include "Diagnostics/DiagnosticTypes.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
using namespace cirrus::diagnostics;
constexpr int kIOReturnOffline=-1,kIOReturnTimeout=-2;
struct IOLock{};
bool locked=false,failString=false;int allocation=0,failAllocation=0;unsigned live=0,clockCalls=0;
void IOLockLock(IOLock*){assert(!locked);locked=true;}
void IOLockUnlock(IOLock*){assert(locked);locked=false;}
void* IOMallocData(size_t n){assert(!locked);if(++allocation==failAllocation)return nullptr;++live;return malloc(n);}
void IOFreeData(void* p,size_t){assert(!locked);--live;free(p);}
void clock_get_uptime(uint64_t* t){++clockCalls;*t=uint64_t(clockCalls)*1000000;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t* out){*out=t;}
#define CIRRUS_LOG(...) do{}while(0)
#define CIRRUS_ERR(...) do{}while(0)
struct OSString{std::string text;static OSString* withCString(const char* s){assert(!locked);return failString?nullptr:new OSString{s};}void release(){delete this;}};
class CirrusAudioFixup{
public:
 static constexpr size_t kTraceBufferSize=1024;
 IOLock* mTraceLock=nullptr;uint32_t mTraceHead=0,mTraceTail=0;TraceStats mTraceStats{};TraceEntry mTraceBuffer[kTraceBufferSize]{};
 struct Native{bool bound(){return false;}}mNativeSPI;
 std::map<std::string,std::string> strings;
 void setProperty(const char* key,OSString* v){assert(!locked);strings[key]=v->text;}
 void setProperty(const char*,uint32_t,unsigned){assert(!locked);}
 void recordTrace(TraceSource,uint8_t,bool,bool,uint32_t,uint32_t,IOReturn);
 void dumpTraceBuffer(const char* propertyName=nullptr,const char* mirrorPropertyName=nullptr);
 void publishStatistics();
};
'''
trace_checks=r'''
int main(){
 CirrusAudioFixup d;
 d.recordTrace(TRACE_PROBE,0,false,false,1,2,0);d.dumpTraceBuffer();assert(clockCalls==0 && allocation==0);
 IOLock lock;d.mTraceLock=&lock;
 for(unsigned i=0;i<1100;++i)d.recordTrace(TRACE_FIRMWARE,0,true,false,i,i+1,-2);
 assert(d.mTraceStats.writeFail==1100 && d.mTraceStats.retries==1100);
 d.dumpTraceBuffer("latest","first");assert(!live && !locked && d.strings["first"]==d.strings["latest"]);
 const auto& text=d.strings["latest"];
 assert(text.find("0x0004D")!=std::string::npos && text.find("0x0044B")!=std::string::npos && text.find("Firmware")!=std::string::npos);
 d.publishStatistics();
 for(int offset:{1,2}){failAllocation=allocation+offset;d.dumpTraceBuffer();assert(!live&&!locked);}
 failAllocation=0;failString=true;d.dumpTraceBuffer();assert(!live&&!locked);
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-trace-') as directory:
    tmp=Path(directory);cpp=tmp/'trace.cpp'
    cpp.write_text(trace_mock+extract('void CirrusAudioFixup::recordTrace(')+extract('void CirrusAudioFixup::dumpTraceBuffer(')+extract('void CirrusAudioFixup::publishStatistics()')+trace_checks)
    for flags in sanitizer_flags():
        subprocess.run(['clang++','-std=c++17',*flags,'-I',str(ROOT/'CirrusAudioFixup'),str(cpp),'-o',str(tmp/'trace')],check=True)
        subprocess.run([str(tmp/'trace')],check=True)
print('PASS production trace: quiet no clock/allocation, bounded chronological wrap, first/latest mirror, unlocked publication and allocation fault cleanup')

publication_mock=r'''
#include "Diagnostics/DiagnosticTypes.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
struct Status{uint32_t stage=6,firstFailureStage=0;int firstFailure=0,latestFailure=0,cleanupResult=0;uint32_t failures=0;};
struct Manager{uint32_t n=2,s=6;Status items[2];size_t count(){return n;}uint32_t stage(){return s;}const Status* status(size_t i){return i<n?&items[i]:nullptr;}const Status* releasedStatus(size_t i){return i<2?&items[i]:nullptr;}};
class CirrusAudioFixup{
public:
 Manager mAmplifierManager;
 cirrus::diagnostics::RuntimeDiagnosticSnapshot mRuntimePublished[16]{};
 uint32_t mPublishedRuntimeCount=UINT32_MAX,mPublishedRuntimeStage=UINT32_MAX;
 unsigned writes=0,stats=0;std::map<std::string,uint64_t> numbers;
 void setProperty(const char* k,uint64_t v,unsigned){++writes;numbers[k]=v;}
 void publishStatistics(){++stats;}
 bool publishRuntimeStatus();
};
'''
publication_checks=r'''
int main(){
 CirrusAudioFixup d;assert(d.publishRuntimeStatus());auto writes=d.writes,stats=d.stats;
 for(unsigned i=0;i<1000;++i)assert(!d.publishRuntimeStatus());assert(d.writes==writes&&d.stats==stats);
 d.mAmplifierManager.items[0].stage=8;assert(d.publishRuntimeStatus());assert(d.numbers["Cirrus_Runtime_0_Stage"]==8&&d.numbers["Cirrus_Runtime_Stage"]==6);
 d.mAmplifierManager.items[1].stage=8;d.mAmplifierManager.s=8;assert(d.publishRuntimeStatus());assert(d.numbers["Cirrus_Runtime_Stage"]==8);
 d.mAmplifierManager.items[0].stage=12;d.mAmplifierManager.s=12;d.mAmplifierManager.items[0].firstFailure=int(0xe00002bcU);d.mAmplifierManager.items[0].latestFailure=-55;d.mAmplifierManager.items[0].cleanupResult=-77;d.mAmplifierManager.items[0].failures=1;
 assert(d.publishRuntimeStatus()&&d.numbers["Cirrus_Runtime_0_FirstFailure"]==0xe00002bcU&&d.numbers["Cirrus_Runtime_0_CleanupReturn"]==uint32_t(-77));
 d.mAmplifierManager.items[0].latestFailure=-88;assert(d.publishRuntimeStatus()&&d.numbers["Cirrus_Runtime_0_FirstFailure"]==0xe00002bcU&&d.numbers["Cirrus_Runtime_0_LatestFailure"]==uint32_t(-88));
 d.mAmplifierManager.s=11;assert(d.publishRuntimeStatus());
 d.mAmplifierManager.items[0].cleanupResult=-99;
 d.mAmplifierManager.n=0;d.mAmplifierManager.s=0;assert(d.publishRuntimeStatus());
 assert(d.numbers["Cirrus_Runtime_0_Bound"]==0&&d.numbers["Cirrus_Runtime_1_Bound"]==0&&d.numbers["Cirrus_Runtime_0_FirstFailure"]==0xe00002bcU);
 assert(d.numbers["Cirrus_Runtime_0_Released_CleanupReturn"]==uint32_t(-99)&&d.numbers["Cirrus_Runtime_0_Released_FirstFailure"]==0xe00002bcU);
 assert(!d.publishRuntimeStatus());
 d.mAmplifierManager.n=1;d.mAmplifierManager.items[0]=Status{};d.mAmplifierManager.s=6;assert(d.publishRuntimeStatus()&&d.numbers["Cirrus_Runtime_0_Bound"]==1);
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-runtime-publication-') as directory:
    p=Path(directory);(p/'publication.cpp').write_text(publication_mock+extract('bool CirrusAudioFixup::publishRuntimeStatus()')+publication_checks)
    for flags in sanitizer_flags():
        subprocess.run(['clang++','-std=c++17',*flags,'-I',str(ROOT/'CirrusAudioFixup'),str(p/'publication.cpp'),'-o',str(p/'publication')],check=True)
        subprocess.run([str(p/'publication')],check=True)
print('PASS runtime transition publication: no steady-state writes, partial/group playback, native 32-bit failures, PM, detach/rebind and preserved root evidence')
