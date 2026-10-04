from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
header = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')
start = source.index('void CirrusAudioFixup::recordDiagnosticFailure(')
function = source[start:source.index('\n}', start) + 2]
states = header[header.index('enum DriverStage'):header.index('struct TraceEntry')]
preamble = r'''
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
using UInt32=uint32_t;
using IOReturn=int;
constexpr int kIOReturnSuccess=0;
#define CIRRUS_ERR(...) (++logs)
struct OSString {
    std::string value;
    static OSString* withCString(const char* s) { return new OSString{s}; }
    void release() { delete this; }
};
'''
mock = r'''
struct CS35L41Amp { const char* name="left"; DiagnosticState diagnostic; };
class CirrusAudioFixup {
public:
    unsigned logs=0,publications=0,snapshots=0,dumps=0;
    bool mCapturingFailureSnapshot=false;
    std::map<std::string,uint32_t> numbers;
    void setProperty(const char* p,uint64_t v,unsigned) { numbers[p]=uint32_t(v); }
    void setProperty(const char*,OSString*) {}
    const char* failureName(DiagnosticFailure) { return "failure"; }
    const char* stageName(DriverStage) { return "stage"; }
    void publishStatistics() { ++publications; }
    void captureFailureSnapshot(CS35L41Amp&,DiagnosticFailure) { ++snapshots; }
    void dumpTraceBuffer(const char*,const char*) { ++dumps; }
    void recordDiagnosticFailure(CS35L41Amp&,DiagnosticFailure,UInt32,UInt32,UInt32,IOReturn,bool);
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
    record(4,-77);
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
    printf("%s diagnostic change publication, rate limiting and snapshot deduplication\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-diagnostics-') as directory:
    tmp = Path(directory)
    cpp = tmp / 'check.cpp'
    exe = tmp / 'check.exe'
    cpp.write_text(preamble + states + mock + function + checks, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
