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
std::vector<std::string> stages;
void trace(IOService*,const char* stage,IOReturn){stages.push_back(stage);}
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
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,trace)==0&&selects.select(0)==nullptr&&selects.select(1));
  assert(stages==std::vector<std::string>({"CS_DSD_BEGIN","CS_DSD_END","CS_CRS_BEGIN","CS_CRS_END",
   "GPIO_MAP_BEGIN","GPIO_MAP_END","GPIO_MMIO_VALIDATE_BEGIN","GPIO_MMIO_VALIDATE_END","CS_INACTIVE_BEGIN","CS_INACTIVE_END"}));
  const auto firmwareCalls=gpio.firmwareCalls;
  assert(selects.validateOutputs()==0&&gpio.firmwareCalls==firmwareCalls);
  gpio.values["_PSC"]=number(3);assert(selects.validateOutputs()==0&&gpio.firmwareCalls==firmwareCalls);
  assert(selects.validatePower()==kIOReturnNotReady&&gpio.firmwareCalls>firmwareCalls);
  gpio.values["_PSC"]=number(0);
  assert(selects.bind(&spi,&owner)==kIOReturnBadArgument);
  assert(selects.select(1)->setActive(true)==0&&mem->r[pad/4]==0x84000200);
  assert(selects.select(1)->setActive(false)==0);gpio.values["_PSC"]=number(3);assert(selects.validate()==kIOReturnNotReady);
  gpio.values["_PSC"]=number(0);assert(selects.validate()==0);}
 assert(!gpio.opened&&gpio.opens==1&&gpio.closes==1&&gpio.refs==1&&mem->refs==1&&dsd->refs==1&&amp.refs==1);
 // Validation failures must not be reclassified as an absent/native-only DSD.
 spi.dsdValidation=kIOReturnUnsupported;const auto opensBefore=gpio.opens;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="DSD_VALIDATION_FAILED");}
 spi.dsdValidation=0;spi.dsdEvaluation=kIOReturnUnsupported;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported&&selects.count()==0);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="DSD_EVALUATION_FAILED");
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Object_Type"]=="NONE");}
 spi.dsdEvaluation=0;
 assert(gpio.opens==opensBefore);
 auto* invalidRoot=make<OSData>(std::vector<uint8_t>{1,2});spi.values["_DSD"]=invalidRoot;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="DSD_ROOT_TYPE");
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Object_Type"]=="OSData");}
 assert(invalidRoot->refs==1);spi.values["_DSD"]=dsd;
 auto* duplicate=properties(array({number(0)}));
 auto* entries=OSDynamicCast(OSArray,duplicate->getObject(1));entries->v.push_back(entries->getObject(0));
 spi.values["_DSD"]=duplicate;
 {ACPIDeviceProperties props;assert(props.load(&spi)==kIOReturnUnsupported);
  assert(!strcmp(props.reason(),"DSD_DUPLICATE_KEY")&&props.get("cs-gpios")==nullptr);}
 spi.values["_DSD"]=dsd;
 // Captured macOS behavior: _DSD evaluates successfully but returns NULL.
 // The explicit data-only adapter preserves references as namespace strings.
 auto* bridge=properties(array({number(0),make<OSString>("\\_SB.PC00.SPI0.GSPK"),number(0),number(0),number(0)}));
 auto* bridgeEntries=OSDynamicCast(OSArray,bridge->getObject(1));
 bridgeEntries->v.push_back(array({make<OSString>("cirrus,acpi-adapter-version"),number(1)}));
 spi.dsdNullResult=true;spi.values["CSDP"]=bridge;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0&&selects.select(1));}
 assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Source"]=="CSDP");
 assert(owner.strings["Cirrus_SPI_DSD_Status"]=="COMPLETE");
 // A changed compatibility HID does not select a physical register layout.
 // Only explicit versioned metadata can preserve the original identity.
 gpio.values["_HID"]=make<OSString>("INT3450");
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 auto* identity=array({dsd->getObject(0),array({
  array({make<OSString>("cirrus,acpi-adapter-version"),number(1)}),
  array({make<OSString>("cirrus,gpio-controller-hid"),make<OSString>("INTC1056")})})});
 gpio.values["CSDP"]=identity;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0&&selects.select(1));}
 assert(owner.strings["Cirrus_SPI_GPIO_HID"]=="INT3450");
 assert(owner.strings["Cirrus_SPI_GPIO_Physical_HID"]=="INTC1056");
 assert(owner.numbers["Cirrus_SPI_GPIO_1_Revision"]==0x940000);
 // No fixed controller or amplifier namespace is required by the adapter.
 spi.path="IOACPIPlane:/_SB/NEW/SPI8";amp.path=spi.path+"/AMPS";
 auto* cs=OSDynamicCast(OSArray,OSDynamicCast(OSArray,bridgeEntries->getObject(0))->getObject(1));
 cs->v[1]=make<OSString>("\\_SB.NEW.SPI8.AMPS");
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 spi.path="IOACPIPlane:/_SB/PC00/SPI0";amp.path=spi.path+"/GSPK";
 cs->v[1]=make<OSString>("\\_SB.PC00.SPI0.GSPK");
 auto* version=OSDynamicCast(OSArray,bridgeEntries->getObject(1));version->v[1]=number(2);
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="ADAPTER_VERSION_UNSUPPORTED");}
 version->v[1]=number(1);
 spi.dsdEvaluation=kIOReturnUnsupported;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 spi.dsdEvaluation=0;
 // A malformed native object is rejected, never hidden by the adapter.
 spi.dsdNullResult=false;spi.values["_DSD"]=invalidRoot;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 spi.values["_DSD"]=dsd;
 gpio.values.erase("CSDP");gpio.values["_HID"]=make<OSString>("INTC1056");
 spi.dsdNullResult=false;spi.values.erase("CSDP");
 // Package cycles/deep/oversized payloads cannot make diagnostic traversal loop.
 auto* cycle=array({});cycle->v.push_back(cycle);
 cirrus::diagnostics::ACPIObjectEvidence::publish(&owner,"Cirrus_Test",cycle);
 assert(owner.strings["Cirrus_Test_Status"]=="PARTIAL_LIMIT"&&owner.numbers["Cirrus_Test_Nodes"]<=256);
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
