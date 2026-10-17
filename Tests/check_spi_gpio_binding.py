"""Run production _DSD -> evaluated _CRS -> GPIO provider/map ownership."""
from pathlib import Path
import subprocess
import tempfile
from acpi_kernel_fixture import install

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cirrus-gpio-binding-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include "Transport/Intel/IntelSPIChipSelects.hpp"
using namespace cirrus::transport;
using namespace cirrus::transport::intel;
OSArray* array(std::initializer_list<OSObject*> values){return make<OSArray>(std::vector<OSObject*>(values));}
OSObject* number(unsigned n){return make<OSNumber>(n);}
OSArray* properties(OSObject* cs){
 auto* id=make<OSData>(std::vector<uint8_t>{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01});
 return array({id,array({array({make<OSString>("cs-gpios"),cs})})});
}
int main(){
 IOACPIPlatformDevice spi,amp,gpio;IOService owner;auto* mem=make<IOMemoryMap>();gpio.map=mem;
 spi.path="IOACPIPlane:/_SB/PC00/SPI0";amp.path=spi.path+"/GSPK";gpio.path="IOACPIPlane:/_SB/GPI0";
 devices={&spi,&amp,&gpio};gpio.values["_HID"]=make<OSString>("INTC1056");gpio.values["_PSC"]=number(0);
 std::vector<uint8_t> crs(25,0);crs[0]=0x8c;crs[3]=1;crs[4]=1;crs[5]=1;crs[7]=2;crs[14]=23;crs[17]=25;crs[23]=19;
 const char path[]="\\_SB.GPI0";crs.insert(crs.end(),path,path+sizeof(path));crs[19]=crs.size();crs[1]=crs.size()-3;crs.insert(crs.end(),{0x79,0});
 amp.values["_CRS"]=make<OSData>(crs);auto* dsd=properties(array({number(0),&amp,number(0),number(0),number(0)}));spi.values["_DSD"]=dsd;
 mem->r[0]=0x940000;mem->r[3]=0x700;const auto pad=0x700+19*16;mem->r[pad/4]=0x84000201;mem->r[0x110/4]=1U<<19;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0&&selects.select(0)==nullptr&&selects.select(1));
  assert(selects.bind(&spi,&owner)==kIOReturnBadArgument);
  assert(selects.select(1)->setActive(true)==0&&mem->r[pad/4]==0x84000200);
  assert(selects.select(1)->setActive(false)==0);gpio.values["_PSC"]=number(3);assert(selects.validate()==kIOReturnNotReady);
  gpio.values["_PSC"]=number(0);assert(selects.validate()==0);}
 assert(!gpio.opened&&gpio.opens==1&&gpio.closes==1&&gpio.refs==1&&mem->refs==1&&dsd->refs==1&&amp.refs==1);
 // Namespace string references use the same resolved device as object references.
 spi.values["_DSD"]=properties(array({number(0),make<OSString>("GSPK"),number(0),number(0),number(1)}));
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 spi.values["_DSD"]=properties(array({number(0),make<OSString>("IOACPIPlane:/_SB/PC00/SPI0/GSPK"),number(0),number(0),number(0)}));
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 // A malformed property must never fall back to a native CS transaction.
 spi.values["_DSD"]=properties(array({number(0),&amp,number(0)}));
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 spi.values["_DSD"]=properties(array({number(0),&amp,number(4),number(0),number(0)}));
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 spi.values["_DSD"]=dsd;gpio.values["_HID"]=make<OSString>("UNKNOWN");
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 gpio.values["_HID"]=make<OSString>("INTC1056");gpio.opened=true;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnExclusiveAccess);}
 gpio.opened=false;assert(gpio.refs==1&&amp.refs==1&&mem->refs==1);
 mem->r[0x114/4]=1U<<19;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnNotPermitted);}
 assert(!gpio.opened&&gpio.refs==1&&mem->refs==1);
}
''')
    for flags in ([], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS production SPI GPIO binding: _DSD references, _CRS, provider exclusivity, maps, PM, failure cleanup")
