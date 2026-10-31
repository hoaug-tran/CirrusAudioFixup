"""ACPI GPIO decoding and Intel output safety using the HP capture's pad states."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/SPI/Intel/IntelGPIOOutput.hpp"
#include "Transport/SPI/Intel/IntelGPIOInput.hpp"
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
 // Meteor Lake-P: Linux pinctrl-meteorlake.c GPIO numbering and PAD_OWN=0xb0.
 assert(lookupIntelGPIOPad("INTC1083",0x82,pad));
 assert(pad.memoryIndex==1&&pad.pad==27&&pad.group==1&&pad.bit==2&&pad.ownershipWord==4&&pad.ownershipBase==0xb0);
 Window mtl;const auto csOffset=0x700+27*16;mtl.r[csOffset/4]=0x84000201;
 // A nonzero old-layout ownership register must not reject this pad.
 mtl.r[(0xa0+4*4)/4]=0xffffffff;
 IntelGPIOOutput csLine;assert(csLine.bind(mtl,sizeof(mtl.r),pad,true)==0);
 assert(csLine.setActive(true)==0&&csLine.setActive(false)==0);
 for(auto offset:mtl.writes)assert(offset==unsigned(csOffset));
 auto writes=mtl.writes.size();mtl.r[(0xb0+4*4)/4]=1U<<8;
 assert(csLine.setActive(true)==kIOReturnNotPermitted&&mtl.writes.size()==writes);
 assert(lookupIntelGPIOPad("INTC1083",0x181,pad));
 assert(pad.memoryIndex==4&&pad.pad==26&&pad.group==1&&pad.bit==1&&pad.ownershipWord==4);
 assert(lookupIntelGPIOPad("INTC1083",0x168,pad));
 assert(pad.memoryIndex==4&&pad.pad==8&&pad.group==0&&pad.bit==8&&pad.ownershipWord==1);
 Window input;const auto inputOffset=0x700+8*16;input.r[inputOffset/4]=0x84000102;
 IntelGPIOInputEvidence evidence;uint32_t level=99;
 input.r[(0xa0+4)/4]=0xffffffff;
 assert(readIntelGPIOInput(input,sizeof(input.r),pad,false,level,evidence)==0&&level==1&&input.writes.empty());
 assert(readIntelGPIOInput(input,sizeof(input.r),pad,true,level,evidence)==0&&level==0);
 input.r[(0xb0+4)/4]=1;
 assert(readIntelGPIOInput(input,sizeof(input.r),pad,false,level,evidence)==kIOReturnNotPermitted&&input.writes.empty());
 for(unsigned pin:{5U,31U,121U,127U,153U,159U,451U,65535U})assert(!lookupIntelGPIOPad("INTC1083",pin,pad));
 assert(!lookupIntelGPIOPad("INTC1082",0x82,pad));
 assert(lookupIntelGPIOPad("INTC105E",0x82,pad));
 // Tiger Lake-LP: Linux pinctrl-tigerlake.c tgllp_soc_data, PAD_OWN=0x20, PADCFGLOCKTX=0x84.
 assert(lookupIntelGPIOPad("INTC1055",304,pad));
 assert(pad.memoryIndex==2&&pad.pad==40&&pad.group==1&&pad.bit==16&&pad.ownershipWord==5&&pad.ownershipBase==0x20&&pad.lockTxBase==0x84);
 assert(lookupIntelGPIOPad("INTC1055",103,pad));
 assert(pad.memoryIndex==1&&pad.pad==7&&pad.group==0&&pad.bit==7&&pad.ownershipWord==0&&pad.ownershipBase==0x20&&pad.lockTxBase==0x84);
 assert(lookupIntelGPIOPad("INTC1055",101,pad));
 assert(pad.memoryIndex==1&&pad.pad==5&&pad.group==0&&pad.bit==5&&pad.ownershipWord==0);
 assert(lookupIntelGPIOPad("INTC1055",327,pad));
 assert(pad.memoryIndex==2&&pad.pad==62&&pad.group==3&&pad.bit==7&&pad.ownershipWord==8);
 assert(lookupIntelGPIOPad("INT34C5",304,pad));
 assert(!lookupIntelGPIOPad("INTC1055",26,pad));
 assert(!lookupIntelGPIOPad("INTC1055",360,pad));
 assert(!lookupIntelGPIOPad("INTC1054",304,pad));
 lookupIntelGPIOPad("INTC1055",304,pad);
 Window tgl;const auto tglCsOffset=0x700+40*16;tgl.r[tglCsOffset/4]=0x84000201;
 tgl.r[(0xa0+4*5)/4]=0xffffffff;tgl.r[(0x114+1*8)/4]=0xffffffff;
 IntelGPIOOutput tglCsLine;assert(tglCsLine.bind(tgl,sizeof(tgl.r),pad,true)==0);
 assert(tglCsLine.setActive(true)==0&&tgl.r[tglCsOffset/4]==0x84000200);
 assert(tglCsLine.setActive(false)==0&&tgl.r[tglCsOffset/4]==0x84000201);
 tgl.r[(0x84+1*8)/4]=1U<<16;
 assert(tglCsLine.setActive(true)==kIOReturnNotPermitted);
 assert(!strcmp(tglCsLine.evidence().reason,"PAD_TX_LOCKED"));
 tgl.r[(0x84+1*8)/4]=0;tgl.r[(0x20+5*4)/4]=1U;
 assert(tglCsLine.setActive(true)==kIOReturnNotPermitted);
 assert(!strcmp(tglCsLine.evidence().reason,"PAD_NOT_HOST_OWNED"));
 Window rev92;rev92.r[0]=0x00920000;rev92.r[3]=0x700;rev92.r[tglCsOffset/4]=0x84000201;
 assert(IntelGPIOOutput{}.bind(rev92,sizeof(rev92.r),pad,true)==0);
 Window rev91;rev91.r[0]=0x00910000;rev91.r[3]=0x700;rev91.r[tglCsOffset/4]=0x84000201;
 assert(IntelGPIOOutput{}.bind(rev91,sizeof(rev91.r),pad,true)==kIOReturnUnsupported);
 Window newLayout;newLayout.r[0]=0x01100000;
 assert(IntelGPIOOutput{}.bind(newLayout,sizeof(newLayout.r),pad,true)==kIOReturnUnsupported);
}
''')
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS ACPI GPIO resource bounds, Intel pad mapping, locked-config TX, ownership and failure readback")
