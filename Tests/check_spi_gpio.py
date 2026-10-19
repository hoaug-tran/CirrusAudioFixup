"""ACPI GPIO decoding and Intel output safety using the HP capture's pad states."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cirrus-spi-gpio-") as directory:
    tmp = Path(directory)
    (tmp / "IOKit").mkdir()
    (tmp / "IOKit/IOLib.h").write_text("""#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnUnsupported=-3,kIOReturnNoDevice=-5,kIOReturnIOError=-6,kIOReturnNotPermitted=-8;
""")
    (tmp / "test.cpp").write_text(r'''
#include <cassert>
#include <cstring>
#include <vector>
#include "Transport/ACPIGPIOResources.hpp"
#include "Transport/Intel/IntelGPIOOutput.hpp"
using namespace cirrus::transport;
using namespace cirrus::transport::intel;
std::vector<uint8_t> descriptor(uint16_t pin) {
 std::vector<uint8_t> d(25,0);const char source[]="\\_SB.GPI0";
 d[0]=0x8c;d[3]=1;d[4]=1;d[5]=1;d[7]=2;d[14]=23;d[17]=25;
 d[23]=pin;d[24]=pin>>8;d.insert(d.end(),source,source+sizeof(source));
 d[19]=d.size();d[1]=d.size()-3;return d;
}
struct Window:IntelGPIOWindow {
 uint32_t r[0x10000/4]{};std::vector<uint32_t> writes;bool blocked=false;
 Window(){r[0]=0x00940000;r[3]=0x700;}
 uint32_t read32(uint32_t o)override{return r[o/4];}
 void write32(uint32_t o,uint32_t v)override{writes.push_back(o);if(!blocked)r[o/4]=v;}
};
int main(){
 auto cs=descriptor(19),reset=descriptor(8);auto bytes=cs;
 bytes.insert(bytes.end(),reset.begin(),reset.end());bytes.insert(bytes.end(),{0x79,0});
 ACPIGPIOResource output{};assert(parseACPIGPIOResource(bytes.data(),bytes.size(),0,0,output));
 assert(output.pin==19&&!strcmp(output.source,"\\_SB.GPI0"));
 assert(parseACPIGPIOResource(bytes.data(),bytes.size(),1,0,output)&&output.pin==8);
 auto unchanged=output;
 assert(!parseACPIGPIOResource(bytes.data(),bytes.size(),2,0,output));
 assert(!parseACPIGPIOResource(bytes.data(),bytes.size(),0,1,output));
 for(size_t n=0;n<bytes.size();++n)assert(!parseACPIGPIOResource(bytes.data(),n,0,0,output));
 assert(!memcmp(&unchanged,&output,sizeof(output)));
 auto bad=bytes;bad[4]=0;assert(!parseACPIGPIOResource(bad.data(),bad.size(),0,0,output));
 bad=bytes;bad[5]=0;assert(!parseACPIGPIOResource(bad.data(),bad.size(),0,0,output));
 bad=bytes;bad[7]|=8;assert(!parseACPIGPIOResource(bad.data(),bad.size(),0,0,output));
 bad=bytes;bad[17]=0xff;assert(!parseACPIGPIOResource(bad.data(),bad.size(),0,0,output));
 bad=bytes;bad[bad.size()-1]=1;assert(!parseACPIGPIOResource(bad.data(),bad.size(),0,0,output));
 IntelGPIOPad pad{};assert(lookupIntelGPIOPad("INTC1056",19,pad)&&pad.memoryIndex==0&&pad.pad==19);
 assert(!lookupIntelGPIOPad("INTC1057",19,pad));
 assert(!lookupIntelGPIOPad("INTC1056",31,pad));
 assert(lookupIntelGPIOPad("INTC1085",224,pad)&&pad.memoryIndex==1&&pad.pad==32&&pad.group==2);
 assert(lookupIntelGPIOPad("INTC1056",19,pad));
 Window w;auto off=0x700+19*16;w.r[off/4]=0x84000201;w.r[0x110/4]=1U<<19;
 IntelGPIOOutput line;assert(line.bind(w,sizeof(w.r),pad,true)==0&&w.writes.empty());
 // Configuration is locked as in the capture, but TX remains writable.
 assert(line.setActive(true)==0&&w.r[off/4]==0x84000200);
 assert(line.setActive(false)==0&&w.r[off/4]==0x84000201);
 assert(line.evidence().logical1Written==0x84000200&&line.evidence().logical1Readback==0x84000200);
 assert(line.evidence().logical0Written==0x84000201&&line.evidence().logical0Readback==0x84000201);
 for(auto o:w.writes)assert(o==unsigned(off));
 auto n=w.writes.size();w.r[0x114/4]=1U<<19;
 assert(line.setActive(true)==kIOReturnNotPermitted&&w.writes.size()==n);
 assert(!strcmp(line.evidence().reason,"PAD_TX_LOCKED"));
 w.r[0x114/4]=0;w.r[(0xa0+8)/4]=1U<<12;
 assert(line.setActive(true)==kIOReturnNotPermitted&&w.writes.size()==n);
 assert(!strcmp(line.evidence().reason,"PAD_NOT_HOST_OWNED"));
 w.r[(0xa0+8)/4]=0;w.r[off/4]|=1U<<8;
 assert(line.setActive(true)==kIOReturnNotPermitted&&w.writes.size()==n);
 assert(!strcmp(line.evidence().reason,"PAD_NOT_GPIO_OUTPUT"));
 w.r[off/4]=0x84000201;w.blocked=true;assert(line.setActive(true)==kIOReturnIOError);
 assert(!strcmp(line.evidence().reason,"PAD_WRITE_READBACK_FAILED"));
 line.unbind();assert(line.setActive(false)==kIOReturnNotReady);
 Window shortMap;IntelGPIOOutput invalid;assert(invalid.bind(shortMap,0x710,pad,true)==kIOReturnUnsupported);
}
''')
    for flags in ([], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS ACPI GPIO resource bounds, Intel pad mapping, locked-config TX, ownership and failure readback")
