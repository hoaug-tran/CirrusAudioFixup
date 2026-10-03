from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
start = SOURCE.index('struct cs35l41_amp_cal_data {')
end = SOURCE.index('\nvoid CirrusAudioFixup::scheduleReadOnlyProbe', start)
calibration = SOURCE[start:end]
gate_start = SOURCE.index('    if (!applyCalibration(amp, image))')
gate_end = SOURCE.index('    bool booted = bringupDSP(amp);', gate_start) + len('    bool booted = bringupDSP(amp);')
gate = SOURCE[gate_start:gate_end]
preamble = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
#define OSDynamicCast(T, p) dynamic_cast<T*>(p)
#define OSSwapLittleToHostInt16(x) (x)
#define OSSwapLittleToHostInt32(x) (x)
constexpr unsigned CS35L41_I2C_ADDR_LEFT=0x40, CS35L41_I2C_ADDR_RIGHT=0x41;
constexpr unsigned WMFW_ADSP2_XM=5, TRACE_FIRMWARE=3, DIAG_CALIBRATION=30;
struct OSObject { virtual ~OSObject()=default; };
struct OSString : OSObject {
    std::string value;
    static OSString* withCString(const char* v) { auto p=new OSString; p->value=v; return p; }
    void release() { delete this; }
};
struct OSData : OSObject {
    std::vector<uint8_t> bytes;
    uint32_t getLength() { return bytes.size(); }
    const void* getBytesNoCopy() { return bytes.data(); }
};
struct IORegistryEntry {
    static OSObject* data;
    static IORegistryEntry* fromPath(const char*) { static IORegistryEntry r; return &r; }
    OSObject* getProperty(const char*) { return data; }
    void release() {}
};
OSObject* IORegistryEntry::data=nullptr;
std::map<std::string,uint32_t> args;
bool PE_parse_boot_argn(const char* name,void* p,size_t n) {
    auto it=args.find(name); if(it==args.end()) return false;
    assert(n==4); memcpy(p,&it->second,4); return true;
}
struct FirmwareImage {};
struct Algorithm { uint32_t id=0xCD; };
struct Control { uint32_t type=5,len=4; };
struct WMFWControlRef { Algorithm* algorithm; Control* control; uint32_t address; };
struct CirrusFirmwareParser {
    static bool missing;
    static bool findControl(const FirmwareImage*,const char* n,WMFWControlRef& r) {
        static Algorithm a; static Control c; r.algorithm=&a; r.control=&c;
        r.address=strcmp(n,"CAL_AMBIENT")==0?0:strcmp(n,"CAL_R")==0?4:strcmp(n,"CAL_STATUS")==0?8:12;
        return !missing;
    }
    static bool resolveControl(const FirmwareImage&,WMFWControlRef& r,uint32_t& address) { address=r.address; return true; }
};
bool CirrusFirmwareParser::missing=false;
struct CS35L41Amp {
    uint8_t address=0x40; const char* name="L";
    bool firmwareValidated=false,playbackFaulted=false;
};
void IOFree(void*,size_t) {}
class CirrusAudioFixup {
public:
    bool skip=false,booted=false,stopped=false;
    unsigned writes=0,reads=0,failWrite=0,failRead=0,corruptRead=0,failures=0;
    std::map<uint32_t,uint32_t> memory;
    std::map<std::string,std::string> properties;
    bool bootArgEnabled(const char*) { return skip; }
    void setProperty(const char* n,OSString* s) { properties[n]=s->value; }
    void setProperty(const char*,uint64_t,unsigned) {}
    bool writeRegister(CS35L41Amp&,uint32_t r,uint32_t v,unsigned) {
        if(++writes==failWrite) return false; memory[r]=v; return true;
    }
    bool readRegister(CS35L41Amp&,uint32_t r,uint32_t* v,unsigned) {
        if(++reads==failRead) return false; *v=memory[r]; if(reads==corruptRead) *v^=1; return true;
    }
    void recordDiagnosticFailure(CS35L41Amp&,unsigned) { ++failures; }
    bool stopDSP(CS35L41Amp&) { stopped=true; return true; }
    bool bringupDSP(CS35L41Amp&) { booted=true; return true; }
    bool applyCalibration(CS35L41Amp&,const FirmwareImage*);
    void boot(CS35L41Amp& amp,FirmwareImage* image);
};
'''
checks = r'''
unsigned failures=0;
void check(bool condition,const char* name) { if(!condition) { ++failures; printf("FAIL %s\n",name); } }
OSData efi(unsigned count=2) {
    OSData d; d.bytes.resize(48);
    cs35l41_amp_efi_data* p=(cs35l41_amp_efi_data*)d.bytes.data();
    p->size=48; p->count=count;
    p->data[0].calAmbient=23; p->data[0].calStatus=1; p->data[0].calR=5846;
    p->data[1].calAmbient=25; p->data[1].calStatus=1; p->data[1].calR=5933;
    return d;
}
int main() {
    FirmwareImage image; CS35L41Amp amp;
    CirrusAudioFixup absent;
    check(absent.applyCalibration(amp,&image) && absent.writes==0,"missing EFI never invents calibration");
    auto data=efi(); IORegistryEntry::data=&data;
    for(unsigned side=0;side<2;++side) {
        CirrusAudioFixup d; amp.address=side?0x41:0x40;
        check(d.applyCalibration(amp,&image),"valid EFI accepted");
        check(d.memory[0]==(side?25U:23U) && d.memory[4]==(side?5933U:5846U) &&
              d.memory[8]==1 && d.memory[12]==d.memory[4]+1,"EFI channel/ambient/checksum match Linux");
    }
    amp.address=0x40;
    for(unsigned n=0;n<48;++n) {
        auto shortData=data; shortData.bytes.resize(n); IORegistryEntry::data=&shortData;
        CirrusAudioFixup d;
        check(!d.applyCalibration(amp,&image) && d.writes==0,"truncated EFI rejected before writes");
    }
    auto badCount=efi(0xFFFFFFFF); IORegistryEntry::data=&badCount;
    CirrusAudioFixup invalid;
    check(!invalid.applyCalibration(amp,&image) && invalid.writes==0,"oversized EFI count rejected");
    IORegistryEntry::data=&data;
    for(unsigned mode=0;mode<3;++mode) for(unsigned n=1;n<=4;++n) {
        CirrusAudioFixup d;
        if(mode==0) d.failWrite=n; else if(mode==1) d.failRead=n; else d.corruptRead=n;
        amp.firmwareValidated=false; d.boot(amp,&image);
        check(!d.booted && !amp.firmwareValidated && d.stopped && d.failures,"calibration fault blocks DSP boot");
        if(mode==0) check(d.writes==n,"write fault stops subsequent calibration writes");
    }
    IORegistryEntry::data=nullptr;
    args={{"cirrus_cal_r0_l",5846}};
    CirrusAudioFixup missingAmbient;
    check(!missingAmbient.applyCalibration(amp,&image) && !missingAmbient.writes,"boot override requires measured ambient");
    args["cirrus_cal_ambient"]=23;
    CirrusAudioFixup explicitData;
    check(explicitData.applyCalibration(amp,&image) && explicitData.memory[4]==5846,"explicit calibration accepted");
    for(auto invalidR:{0U,65536U,0xFFFFFFFFU}) {
        args["cirrus_cal_r0_l"]=invalidR; CirrusAudioFixup d;
        check(!d.applyCalibration(amp,&image) && !d.writes,"invalid R0 rejected");
    }
    args["cirrus_cal_r0_l"]=5846; args["cirrus_cal_ambient"]=128;
    CirrusAudioFixup badAmbient;
    check(!badAmbient.applyCalibration(amp,&image) && !badAmbient.writes,"ambient must fit EFI signed byte");
    args.clear(); IORegistryEntry::data=&data;
    CirrusFirmwareParser::missing=true; CirrusAudioFixup missingControl;
    missingControl.boot(amp,&image);
    check(!missingControl.booted && missingControl.failures,"missing control blocks boot");
    CirrusFirmwareParser::missing=false;
    CirrusAudioFixup normal; normal.boot(amp,&image);
    check(normal.booted && amp.firmwareValidated,"valid calibration permits boot");
    printf("%s calibration: EFI bounds, L/R values, 12 I/O faults, boot gate, explicit input validation\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-calibration-') as directory:
    tmp = Path(directory)
    source = tmp / 'check.cpp'
    source.write_text(preamble + calibration + '\nvoid CirrusAudioFixup::boot(CS35L41Amp& amp,FirmwareImage* image) {\n' + gate + '\n}\n' + checks, encoding='utf-8')
    exe = tmp / 'check.exe'
    subprocess.run(['g++', '-std=c++17', '-O0', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
