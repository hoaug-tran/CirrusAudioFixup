import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
start = re.search(r'struct\s+cs35l41_amp_cal_data\s*\{', SOURCE).start()
end = start + re.search(r'\nvoid\s+CirrusAudioFixup::scheduleReadOnlyProbe', SOURCE[start:]).start()
calibration = SOURCE[start:end]
gate_start = re.search(r'if\s*\(!applyCalibration\(amp,\s*image\)\)', SOURCE).start()
m_boot = re.search(r'bool\s+booted\s*=\s*bringupDSP\(amp\);', SOURCE[gate_start:])
gate_end = gate_start + m_boot.end()
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
unsigned borrowedReads=0,outstandingRefs=0;
struct OSObject {
    unsigned refs=1;
    virtual ~OSObject()=default;
    void retain() { ++refs; ++outstandingRefs; }
    void release() { assert(refs>1); --refs; --outstandingRefs; }
};
struct OSString : OSObject {
    std::string value;
    static OSString* withCString(const char* v) { auto p=new OSString; p->value=v; return p; }
    void release() { delete this; }
};
struct OSData : OSObject {
    std::vector<uint8_t> bytes;
    uint32_t getLength() { if(refs<2) ++borrowedReads; return bytes.size(); }
    const void* getBytesNoCopy() { if(refs<2) ++borrowedReads; return bytes.data(); }
};
struct IORegistryEntry {
    static OSObject* data;
    static bool legacyOnly;
    static IORegistryEntry* fromPath(const char*) { static IORegistryEntry r; return &r; }
    OSObject* getProperty(const char* name) { return legacyOnly && strchr(name,':') ? nullptr : data; }
    OSObject* copyProperty(const char* name) { auto p=getProperty(name); if(p) p->retain(); return p; }
    void release() {}
};
OSObject* IORegistryEntry::data=nullptr;
bool IORegistryEntry::legacyOnly=false;
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
using AmplifierState = CS35L41Amp;
namespace cirrus { namespace devices { namespace cs35l41 { namespace registers { constexpr uint8_t kI2cAddressRight = 0x41; } } } }
unsigned freedImages=0;
void IOFree(void*,size_t) { ++freedImages; }
class CirrusAudioFixup {
public:
    bool skip=false,booted=false,stopped=false;
    unsigned writes=0,reads=0,failWrite=0,failRead=0,corruptRead=0,failures=0;
    std::map<uint32_t,uint32_t> memory;
    std::map<std::string,std::string> properties;
    std::map<std::string,uint64_t> numbers;
    bool bootArgEnabled(const char*) { return skip; }
    void setProperty(const char* n,OSString* s) { properties[n]=s->value; }
    void setProperty(const char* n,uint64_t v,unsigned) { numbers[n]=v; }
    void removeProperty(const char* n) { numbers.erase(n); properties.erase(n); }
    bool writeRegister(AmplifierState&,uint32_t r,uint32_t v,unsigned) {
        if(++writes==failWrite) return false; memory[r]=v; return true;
    }
    bool readRegister(AmplifierState&,uint32_t r,uint32_t* v,unsigned) {
        if(++reads==failRead) return false; *v=memory[r]; if(reads==corruptRead) *v^=1; return true;
    }
    void recordDiagnosticFailure(AmplifierState&,unsigned) { ++failures; }
    bool stopDSP(AmplifierState&) { stopped=true; return true; }
    bool bringupDSP(AmplifierState&) { booted=true; return true; }
    bool phaseHalted=false;
    bool stopAfterDebugStage(const char*) { return phaseHalted; }
    bool applyCalibration(AmplifierState&,const FirmwareImage*);
    void boot(AmplifierState& amp,FirmwareImage* image);
};
'''
checks = r'''
unsigned failures=0;
void check(bool condition,const char* name) { if(!condition) { ++failures; printf("FAIL %s\n",name); } }
OSData efi(unsigned count=2) {
    OSData d; d.bytes.resize(48);
    cs35l41_amp_efi_data* p=(cs35l41_amp_efi_data*)d.bytes.data();
    p->size=48; p->count=count;
    p->data[0].calTime[0]=1; p->data[1].calTime[0]=1;
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
    auto targeted=data;
    auto* targets=(cs35l41_amp_efi_data*)targeted.bytes.data();
    targets->data[0].calTarget[0]=0x55667788; targets->data[0].calTarget[1]=0x11223344;
    targets->data[1].calTarget[0]=0xDDEEFF00; targets->data[1].calTarget[1]=0x99AABBCC;
    IORegistryEntry::data=&targeted;
    CirrusAudioFixup reordered;
    reordered.memory[0x17044]=0x99AABBCC; reordered.memory[0x17040]=0xDDEEFF00;
    check(reordered.applyCalibration(amp,&image) && reordered.memory[4]==5933,
          "silicon UID takes precedence over channel index");
    CirrusAudioFixup mismatched;
    mismatched.memory[0x17044]=0x12345678; mismatched.memory[0x17040]=1;
    check(mismatched.applyCalibration(amp,&image) && mismatched.writes==0,
          "unmatched nonzero target is never applied to another speaker");
    CirrusAudioFixup zeroUid;
    check(zeroUid.applyCalibration(amp,&image) && zeroUid.memory[4]==5846,
          "zero silicon UID permits Linux index fallback");
    targets->data[1].calTime[0]=0;
    CirrusAudioFixup emptyTarget;
    emptyTarget.memory[0x17044]=0x99AABBCC; emptyTarget.memory[0x17040]=0xDDEEFF00;
    check(emptyTarget.applyCalibration(amp,&image) && emptyTarget.writes==0,
          "UID match cannot revive an unused timestamp slot");
    targets->data[1].calTime[0]=1;
    for(unsigned n=1;n<=2;++n) {
        CirrusAudioFixup d; d.failRead=n;
        check(!d.applyCalibration(amp,&image) && d.writes==0,
              "UID read failure blocks calibration writes");
        check(d.properties["Cirrus_Calibration_Status_L"]=="UID_READ_FAILED",
              "UID transport failure is not reported as missing calibration");
    }
    targets->data[0].calTarget[0]=targets->data[0].calTarget[1]=0;
    CirrusAudioFixup wildcard;
    wildcard.memory[0x17044]=0x12345678; wildcard.memory[0x17040]=1;
    check(wildcard.applyCalibration(amp,&image) && wildcard.memory[4]==5846,
          "wildcard entry permits index fallback");
    auto singleton=targeted;
    auto* single=(cs35l41_amp_efi_data*)singleton.bytes.data();
    single->count=1; single->data[0]=targets->data[1];
    IORegistryEntry::data=&singleton;
    amp.address=0x41;
    CirrusAudioFixup outsideIndex;
    outsideIndex.memory[0x17044]=0x99AABBCC; outsideIndex.memory[0x17040]=0xDDEEFF00;
    check(outsideIndex.applyCalibration(amp,&image) && outsideIndex.memory[4]==5933,
          "UID lookup works even when channel index exceeds entry count");
    amp.address=0x40;
    auto emptySlot=data;
    ((cs35l41_amp_efi_data*)emptySlot.bytes.data())->data[0].calTime[0]=0;
    IORegistryEntry::data=&emptySlot;
    CirrusAudioFixup unused;
    check(unused.applyCalibration(amp,&image) && unused.writes==0,
          "unused EFI slot leaves firmware defaults untouched");
    check(unused.properties["Cirrus_Calibration_Status_L"]=="NOT_AVAILABLE",
          "unused EFI slot is not reported as applied calibration");
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
    args={{"-cirruscalr0l",5846}};
    CirrusAudioFixup missingAmbient;
    check(!missingAmbient.applyCalibration(amp,&image) && !missingAmbient.writes,"boot override requires measured ambient");
    args["-cirruscalambient"]=23;
    CirrusAudioFixup explicitData;
    check(explicitData.applyCalibration(amp,&image) && explicitData.memory[4]==5846,"explicit calibration accepted");
    for(auto invalidR:{0U,65536U,0xFFFFFFFFU}) {
        args["-cirruscalr0l"]=invalidR; CirrusAudioFixup d;
        check(!d.applyCalibration(amp,&image) && !d.writes,"invalid R0 rejected");
    }
    args["-cirruscalr0l"]=5846; args["-cirruscalambient"]=128;
    CirrusAudioFixup badAmbient;
    check(!badAmbient.applyCalibration(amp,&image) && !badAmbient.writes,"ambient must fit EFI signed byte");
    args.clear(); IORegistryEntry::data=&data;
    CirrusFirmwareParser::missing=true; CirrusAudioFixup missingControl;
    missingControl.boot(amp,&image);
    check(!missingControl.booted && missingControl.failures,"missing control blocks boot");
    CirrusFirmwareParser::missing=false;
    CirrusAudioFixup normal; normal.boot(amp,&image);
    check(normal.booted && amp.firmwareValidated,"valid calibration permits boot");
    unsigned beforePhase=freedImages;
    CirrusAudioFixup phase; phase.phaseHalted=true; phase.boot(amp,&image);
    check(amp.firmwareValidated && !phase.booted,"firmware phase validates but never boots DSP");
    check(freedImages==beforePhase+1,"firmware debug halt releases the parsed image");
    IORegistryEntry::legacyOnly=true;
    CirrusAudioFixup legacy;
    check(legacy.applyCalibration(amp,&image),"unqualified EFI property fallback accepted");
    IORegistryEntry::legacyOnly=false;
    OSObject wrongType;
    IORegistryEntry::data=&wrongType;
    CirrusAudioFixup wrong;
    check(!wrong.applyCalibration(amp,&image) && wrong.writes==0,"wrong EFI property type rejected");
    check(wrongType.refs==1,"wrong-type property released");
    const char* expectedStatuses[]={"NOT_AVAILABLE","SKIPPED_BY_BOOT_ARG","CONTROL_RESOLUTION_FAILED",
        "INVALID_EFI_DATA","INVALID_BOOT_ARG_DATA","VERIFY_FAILED","VERIFY_FAILED","VERIFY_FAILED"};
    for(unsigned mode=0;mode<9;++mode) {
        args.clear(); IORegistryEntry::data=&data; CirrusFirmwareParser::missing=false;
        CirrusAudioFixup d;
        CS35L41Amp right; right.name="R"; right.address=0x41;
        check(d.applyCalibration(amp,&image) && d.applyCalibration(right,&image),"seed both channel properties");
        check(d.numbers.size()==8,"both channels publish four verified values");
        if(mode==0) IORegistryEntry::data=nullptr;
        if(mode==1) d.skip=true;
        if(mode==2) CirrusFirmwareParser::missing=true;
        if(mode==3) IORegistryEntry::data=&wrongType;
        if(mode==4) { IORegistryEntry::data=nullptr; args={{"-cirruscalr0l",5846}}; }
        if(mode==5) d.failWrite=d.writes+1;
        if(mode==6) d.failRead=d.reads+1;
        if(mode==7) d.corruptRead=d.reads+1;
        bool ok=d.applyCalibration(amp,mode==8?nullptr:&image);
        check(ok==(mode<2),"repeat invocation preserves success/failure contract");
        for(auto field:{"R0","Ambient","Valid","Checksum"}) {
            check(!d.numbers.count(std::string("Cirrus_Calibration_")+field+"_L"),"repeat invocation clears old channel evidence");
            check(d.numbers.count(std::string("Cirrus_Calibration_")+field+"_R")==1,"other channel evidence preserved");
        }
        if(mode<8) check(d.properties["Cirrus_Calibration_Status_L"]==expectedStatuses[mode],"repeat status describes current attempt");
        else check(!d.properties.count("Cirrus_Calibration_Status_L"),"null image cannot retain successful status");
        d.skip=false; d.failWrite=d.failRead=d.corruptRead=0;
        args.clear(); IORegistryEntry::data=&data; CirrusFirmwareParser::missing=false;
        check(d.applyCalibration(amp,&image) && d.numbers.size()==8,"successful retry republishes current evidence");
    }
    check(borrowedReads==0,"EFI data retained throughout parsing");
    check(outstandingRefs==0 && data.refs==1 && wrongType.refs==1,"property references balanced across all paths");
    printf("%s calibration: EFI bounds, UID priority/wildcards/mismatch, UID read faults, ownership, L/R values, 12 I/O faults, boot gate, repeated state transitions\n",failures?"FAIL":"PASS");
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
