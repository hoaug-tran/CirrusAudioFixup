import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
from acpi_kernel_fixture import install

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-cs35l56-metadata-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include "Fixture.hpp"
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Transport/SPI/Intel/IntelSPIChipSelects.hpp"
using namespace cirrus;
namespace cirrus::transport {
ACPIPropertyEvaluator nativePropertyEvaluator{nullptr};
const char* nativePropertySetupState{"CALLBACK_NOT_REACHED"};
}
int main() {
 cirrus::transport::nativePropertyEvaluator=[](IOACPIPlatformDevice* device,OSObject** out)->IOReturn {
  auto found=device->values.find("INTERPRETER_DSD");
  if(found==device->values.end())return kIOReturnUnsupported;
  *out=found->second;(*out)->retain();return kIOReturnSuccess;
 };

    IOACPIPlatformDevice spi, amp, gpio;
    IOService owner;
    spi.path="IOACPIPlane:/_SB/PC00/SPI0";
    amp.path=spi.path+"/SPK1";
    gpio.path="IOACPIPlane:/_SB/GPI0";
    gpio.values["_HID"]=make<OSString>("INTC1083");
    ::devices={&spi,&amp,&gpio};
    const char* hex="8e210002000202000001090000093d0008000000005c5f53422e504330302e5350493000"
                    "8e210002000202000001090000093d0008000001005c5f53422e504330302e5350493000"
                    "8c2000010101000200010000000017000019002300000082005c5f53422e4750493000"
                    "8c2000010101000200020000000017000019002300000081015c5f53422e4750493000"
                    "8c2000010101000100010000000017000019002300000068015c5f53422e47504930007900";
    std::vector<uint8_t> bytes;
    for(size_t i=0;hex[i];i+=2){unsigned n=0;assert(sscanf(hex+i,"%2x",&n)==1);bytes.push_back(uint8_t(n));}
    amp.values["_CRS"]=make<OSData>(bytes);
    auto array=[](std::initializer_list<OSObject*> values){return make<OSArray>(std::vector<OSObject*>(values));};
    auto number=[](uint32_t n)->OSObject*{return make<OSNumber>(n);};
    const std::vector<uint8_t> uuid{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01};

    auto* csProperty=array({make<OSString>("cs-gpios"),array({number(0),make<OSString>("\\\\_SB.PC00.SPI0.SPK1"),number(0),number(0),number(0)})});
    auto* speaker=array({make<OSString>("spk-id-gpios"),array({make<OSString>("\\\\_SB.PC00.SPI0.SPK1"),number(2),number(0),number(0),make<OSString>("\\\\_SB.PC00.SPI0.SPK1"),number(2),number(0),number(0)})});
    spi.values["INTERPRETER_DSD"]=array({make<OSData>(uuid),array({csProperty})});
    amp.values["INTERPRETER_DSD"]=array({make<OSData>(uuid),array({speaker})});
    spi.values["_DSD"]=amp.values["_DSD"]=make<OSNumber>(0);
    spi.dsdNullResult=amp.dsdNullResult=true;
    for(auto* device:{&spi,&amp}) {
        transport::ACPIDeviceProperties properties;
        assert(properties.load(device)==kIOReturnSuccess);
        assert(!strcmp(properties.source(),"_DSD_INTERPRETER"));
        assert(properties.get(device==&spi?"cs-gpios":"spk-id-gpios"));
    }
    auto* cs=make<IOMemoryMap>();
    cs->r[0]=0x960000;cs->r[3]=0x700;
    cs->r[(0x700+27*16)/4]=0x84000201;
    gpio.maps={{1,cs}};
    {
        transport::intel::IntelSPIChipSelects selects;
        assert(selects.bind(&spi,&owner)==0);
        assert(selects.count()==2&&!selects.select(0)&&selects.select(1));
        assert(selects.pin(1)==0x82);
        assert(cs->r[(0x700+27*16)/4]==0x84000201);
    }
    assert(!gpio.opened&&gpio.opens==gpio.closes);
    amp.values["INTERPRETER_DSD"]=make<OSNumber>(0);
    transport::ACPIDeviceProperties invalid;
    assert(invalid.load(&amp)!=kIOReturnSuccess);
    amp.values.erase("INTERPRETER_DSD");
    transport::ACPIDeviceProperties missing;
    assert(missing.load(&amp)!=kIOReturnSuccess);

}
''')
    executable = tmp / "test.exe"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I"+str(tmp),
                    "-I"+str(ROOT/"CirrusAudioFixup"), str(tmp/"test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS CS35L56 interpreter references, native/GPIO CS, unchanged pad state and missing/invalid metadata rejection")
