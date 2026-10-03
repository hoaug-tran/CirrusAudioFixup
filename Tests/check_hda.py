from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')


def function(name):
    start = re.search(r'bool CirrusAudioFixup::' + name + r'\(', SOURCE).start()
    return SOURCE[start:SOURCE.index('\n}', start) + 2]


preamble = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <map>
#include <cstring>
using UInt32=uint32_t;
constexpr int kIOReturnSuccess=0,kIOReturnNotFound=-1;
constexpr int STAGE_HDA_DETECT=0,DIAG_HDA_CONTROLLER=1,DIAG_HDA_STREAM_FORMAT=2;
constexpr int kIOPCIConfigVendorID=0,kIOPCIConfigDeviceID=2;
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
#define OSDynamicCast(T,p) dynamic_cast<T*>(p)
struct OSString {
    std::string value;
    static OSString* withCString(const char* s) { return new OSString{s}; }
    void release() { delete this; }
};
struct IOMemoryMap {
    alignas(8) uint8_t bytes[0x900]{};
    unsigned releases=0;
    size_t length=sizeof(bytes);
    bool nullAddress=false;
    uintptr_t getVirtualAddress() { return nullAddress?0:reinterpret_cast<uintptr_t>(bytes); }
    size_t getLength() { return length; }
    void release() { ++releases; }
};
struct IOService { virtual ~IOService()=default; unsigned releases=0; void release() { ++releases; } };
struct IOPCIDevice:IOService {
    IOMemoryMap bar;
    bool noMap=false;
    IOMemoryMap* mapDeviceMemoryWithRegister(unsigned) { return noMap?nullptr:&bar; }
    const char* getName() { return "fake-HDEF"; }
    uint16_t configRead16(unsigned r) { return r==0?0x1022:0x15e3; }
};
class CirrusAudioFixup {
public:
    bool mHdaConverterPrepared=false,mHdaControllerObserved=false,mHdaStreamActive=false;
    bool mPowerAvailable=true,mStopping=false,mHdaTopologyLogged=false,missing=false;
    uint8_t mHdaLastDescriptor=0xFF,mHdaLastStreamTag=0;
    uint16_t mHdaLastFormat=0;
    unsigned mHdaMissCount=0,lookups=0;
    int mAmps[2]{};
    IOPCIDevice pci;
    std::map<std::string,std::string> properties;
    void write16(unsigned offset,uint16_t v) { memcpy(pci.bar.bytes+offset,&v,2); }
    void write32(unsigned offset,uint32_t v) { memcpy(pci.bar.bytes+offset,&v,4); }
    void stream(unsigned index,unsigned tag,bool output=true,uint16_t format=0x31) {
        write32(0x80+index*0x20,(tag<<20)|(output?1U<<19:0)|2);
        write16(0x80+index*0x20+0x12,format);
    }
    CirrusAudioFixup() { write16(0,0x1000); write32(8,1); stream(0,1); }
    IOService* getAudioController() { ++lookups; return missing?nullptr:&pci; }
    void setProperty(const char* p,OSString* v) { properties[p]=v->value; }
    void setDiagnosticStage(int&,int) {}
    template<class... T> void recordDiagnosticFailure(int&,T...) {}
    bool syncAlc287HdaCodec();
    static bool supportedHdaFormat(uint16_t);
};
unsigned failures=0;
void check(bool ok,const char* label) { if(!ok) { ++failures; fprintf(stderr,"FAIL %s\n",label); } }
'''
checks = r'''
int main() {
    CirrusAudioFixup normal;
    check(normal.syncAlc287HdaCodec() && normal.mHdaLastDescriptor==0 && normal.mHdaLastStreamTag==1,"fixed output stream");
    normal.write32(0x80,0);
    check(!normal.syncAlc287HdaCodec() && normal.mHdaLastStreamTag==0,"RUN cleared invalidates stream");
    check(normal.pci.releases==2 && normal.pci.bar.releases==2,"balanced PCI and BAR references");
    normal.missing=true;
    check(!normal.syncAlc287HdaCodec() && normal.properties["Cirrus_HDA_Status"]=="MISSING",
          "controller disappearance replaces previous stream status immediately");
    CirrusAudioFixup missing; missing.missing=true;
    for(unsigned i=0;i<20;++i) check(!missing.syncAlc287HdaCodec(),"missing controller");
    check(missing.properties["Cirrus_HDA_Status"]=="MISSING_AFTER_20_POLLS","missing status");
    for(unsigned mode=0;mode<5;++mode) {
        CirrusAudioFixup d;
        if(mode==0) d.pci.noMap=true;
        if(mode==1) d.pci.bar.length=0x40;
        if(mode==2) d.pci.bar.nullAddress=true;
        if(mode==3) d.write16(0,0xFFFF);
        if(mode==4) d.pci.bar.length=0x90;
        check(!d.syncAlc287HdaCodec() && !d.mHdaControllerObserved,"invalid BAR/capability rejected");
        check(d.properties["Cirrus_HDA_Status"]!="OK" && !d.properties["Cirrus_HDA_Status"].empty(),"invalid BAR cannot report OK");
        check(d.pci.releases==1 && d.pci.bar.releases==(mode==0?0U:1U),"failure releases ownership");
    }
    CirrusAudioFixup reset;
    reset.write32(8,0); // GCAP and descriptor content can survive reset.
    check(!reset.syncAlc287HdaCodec() && !reset.mHdaControllerObserved,"controller held in reset cannot be active");
    CirrusAudioFixup bidir;
    bidir.write16(0,0x1108); // one input, one output, one bidirectional
    bidir.write32(0x80,0); bidir.stream(2,7);
    check(bidir.syncAlc287HdaCodec() && bidir.mHdaLastDescriptor==2 && bidir.mHdaLastStreamTag==7,"bidirectional output");
    bidir.stream(2,7,false);
    check(!bidir.syncAlc287HdaCodec(),"bidirectional input cannot trigger speakers");
    CirrusAudioFixup unsupported;
    unsupported.stream(0,1,true,0x4031);
    check(!unsupported.syncAlc287HdaCodec(),"44.1 kHz blocked by fixed 48 kHz ASP");
    CirrusAudioFixup ambiguous;
    ambiguous.write16(0,0x2000); ambiguous.stream(1,2);
    check(!ambiguous.syncAlc287HdaCodec(),"multiple running outputs cannot identify speaker descriptor");
    check(ambiguous.properties["Cirrus_HDA_Status"]=="AMBIGUOUS_OUTPUT_STREAMS","ambiguous route has explicit status");
    CirrusAudioFixup unassigned;
    unassigned.write16(0,0x2000); unassigned.stream(1,0);
    check(unassigned.syncAlc287HdaCodec() && unassigned.mHdaLastDescriptor==0 &&
          unassigned.mHdaLastStreamTag==1,"unassigned later descriptor cannot erase the unique candidate");
    CirrusAudioFixup streamReset;
    streamReset.write32(0x80,(1U<<20)|3);
    check(!streamReset.syncAlc287HdaCodec(),"stream held in reset cannot trigger playback");
    CirrusAudioFixup asleep; asleep.mPowerAvailable=false;
    check(!asleep.syncAlc287HdaCodec() && asleep.lookups==0,"no PCI access while suspended");
    if(failures) return 1;
    puts("PASS HDA BAR bounds/ownership, controller reset, fixed/bidirectional streams, format, ambiguity and power guard");
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-hda-check-') as directory:
    tmp = Path(directory)
    source = tmp / 'hda.cpp'
    source.write_text(preamble + function('syncAlc287HdaCodec') + '\n' + function('supportedHdaFormat') + checks,
                      encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O0', str(source), '-o', str(tmp / 'hda.exe')], check=True)
    subprocess.run([str(tmp / 'hda.exe')], check=True)
