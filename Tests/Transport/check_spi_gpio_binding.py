"""Run production _DSD -> evaluated _CRS -> GPIO provider/map ownership."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
import subprocess
import tempfile
from acpi_kernel_fixture import install

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-gpio-binding-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include "Platform/ACPI/ACPIBoardMetadata.hpp"
#include "Transport/SPI/Intel/IntelSPIChipSelects.hpp"
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
namespace cirrus::transport {
ACPIPropertyEvaluator nativePropertyEvaluator{nullptr};
const char* nativePropertySetupState{"CALLBACK_NOT_REACHED"};
}
int main(){
 cirrus::transport::nativePropertyEvaluator=[](IOACPIPlatformDevice* device,OSObject** out)->IOReturn {
  auto found=device->values.find("INTERPRETER_DSD");
  if(found==device->values.end())return kIOReturnUnsupported;
  *out=found->second;(*out)->retain();return kIOReturnSuccess;
 };

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
 // Issue #5: an unmodified, supported GPIO HID needs no synthetic _DSD.
 // Apple may report Error for an absent object; this optional GPIO metadata
 // must not change the mandatory CS wiring contract on the SPI controller.
 for(auto error:{kIOReturnError,kIOReturnNotFound,kIOReturnLockedWrite}){
  gpio.dsdValidation=error;
  {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0&&selects.select(1));}
  assert(owner.strings["Cirrus_SPI_GPIO_Identity_Source"]=="ACPI_HID");
 }
 gpio.dsdValidation=0;
 // A supported HID never hides malformed metadata or an unreadable package.
 gpio.values["_DSD"]=make<OSData>(std::vector<uint8_t>{1,2});
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 gpio.values.erase("_DSD");gpio.dsdEvaluation=kIOReturnIOError;
 gpio.values["_DSD"]=properties(array({number(0)}));
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnIOError);}
 gpio.values.erase("_DSD");gpio.dsdEvaluation=0;
 // Observe the shared reset reference through the existing exclusive CS map.
 // Both logical levels must be visible without ever changing the reset pad.
 auto resetCrs=crs;resetCrs.resize(resetCrs.size()-2);
 auto resetResource=resetCrs;resetResource[23]=8;
 resetCrs.insert(resetCrs.end(),resetResource.begin(),resetResource.end());
 resetCrs.insert(resetCrs.end(),{0x79,0});amp.values["_CRS"]=make<OSData>(resetCrs);
 auto* ampProperties=properties(array({number(0)}));
 auto* ampEntries=OSDynamicCast(OSArray,ampProperties->getObject(1));
 ampEntries->v.push_back(array({make<OSString>("reset-gpios"),array({&amp,number(1),number(0),number(0),&amp,number(1),number(0),number(0)})}));
 amp.values["_DSD"]=ampProperties;const auto resetPad=0x700+8*16;
 // Replay the captured held-low shared reset. Preparing the group must not
 // drive it; the explicit logical low/high sequence releases it once.
 mem->r[resetPad/4]=0x44000200;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);
  assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==0);
  assert(owner.numbers["Cirrus_SPI_GPIO_Group_Unique_Pins"]==1&&mem->r[resetPad/4]==0x44000200);
  assert(selects.writeGPIOGroup(false)==0&&mem->r[resetPad/4]==0x44000200);
  assert(selects.writeGPIOGroup(true)==0&&mem->r[resetPad/4]==0x44000201);
  selects.publishGPIOGroupEvidence();
  assert(owner.numbers["Cirrus_SPI_GPIO_Group_0_Logical0_Readback"]==0x44000200);
  assert(owner.numbers["Cirrus_SPI_GPIO_Group_0_Logical1_Readback"]==0x44000201);
  assert(mem->r[pad/4]==0x84000201);
  selects.shutdown();assert(selects.writeGPIOGroup(false)==kIOReturnNotReady);}
 // Reject reset/CS aliases before writing; a locked shared reset must not be
 // bypassed. A failure preparing the second tuple cannot drive the first.
 auto* groupTuples=OSDynamicCast(OSArray,OSDynamicCast(OSArray,ampEntries->getObject(1))->getObject(1));
 groupTuples->v[5]=number(0);
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);
  assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==kIOReturnNotPermitted);
  assert(selects.writeGPIOGroup(false)==kIOReturnNotReady&&mem->r[resetPad/4]==0x44000201);}
 groupTuples->v[5]=number(1);mem->r[0x114/4]=1U<<8;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);
  assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==kIOReturnNotPermitted);
  assert(selects.writeGPIOGroup(false)==kIOReturnNotReady&&mem->r[resetPad/4]==0x44000201);}
 mem->r[0x114/4]=0;
 for(unsigned level:{0U,1U}){
  mem->r[resetPad/4]=0x84000200|level;
  {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0&&selects.select(1));
   assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="OBSERVED_WITHOUT_WRITES");
   assert(owner.numbers["Cirrus_SPI_Reset_0_PhysicalLevel"]==level);
   assert(owner.numbers["Cirrus_SPI_Reset_1_LogicalLevel"]==level);
   assert(mem->r[resetPad/4]==(0x84000200|level));}
 }
 // A locked reset pad is reported, never unlocked or driven for diagnosis.
 mem->r[0x114/4]=1U<<8;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
  assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="PAD_TX_LOCKED");}
 mem->r[0x114/4]=0;
 auto* resetTuples=OSDynamicCast(OSArray,OSDynamicCast(OSArray,ampEntries->getObject(1))->getObject(1));
 resetTuples->v[0]=resetTuples->v[4]=make<OSString>("\\_SB.PC00.SPI0.GSPK");
 resetTuples->v[3]=number(1);
 amp.dsdNullResult=true;amp.values["INTERPRETER_DSD"]=ampProperties;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
  assert(owner.strings["Cirrus_SPI_Reset_Properties_Source"]=="_DSD_INTERPRETER");
  assert(owner.numbers["Cirrus_SPI_Reset_0_PhysicalLevel"]==1&&owner.numbers["Cirrus_SPI_Reset_0_LogicalLevel"]==0);
  assert(mem->r[resetPad/4]==0x84000201);}
 amp.dsdNullResult=false;amp.values.erase("INTERPRETER_DSD");
 // Replay issue #5's INTC1085 topology: CS1 GPIO 468 in community 4,
 // shared reset GPIO 320 in community 3. Each map has independent storage.
 // Merely observing/preparing reset may map the owned provider, never pulse it.
 {
  auto* csMap=make<IOMemoryMap>();auto* resetMap=make<IOMemoryMap>();
  for(auto* m:{csMap,resetMap}){m->r[0]=0x960000;m->r[3]=0x700;}
  const auto csOffset=0x700+20*16;const auto resetOffset=0x700;
  csMap->r[csOffset/4]=0x84000201;resetMap->r[resetOffset/4]=0x44000200;
  // unrelated locked pad must not block GPIO 468
  csMap->r[0x114/4]=1U<<14;
  auto asusCS=crs;asusCS.resize(asusCS.size()-2);asusCS[23]=0xd4;asusCS[24]=1;
  auto asusReset=asusCS;asusReset[23]=0x40;asusReset[24]=1;
  asusCS.insert(asusCS.end(),asusReset.begin(),asusReset.end());asusCS.insert(asusCS.end(),{0x79,0});
  amp.values["_CRS"]=make<OSData>(asusCS);
  resetTuples->v[3]=number(0);
  gpio.values["_HID"]=make<OSString>("INTC1085");gpio.maps={{4,csMap},{3,resetMap}};
  const auto opens=gpio.opens;
  {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
   assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="OBSERVED_WITHOUT_WRITES");
   assert(owner.numbers["Cirrus_SPI_Reset_0_Memory_Index"]==3);
   assert(owner.numbers["Cirrus_SPI_Reset_0_PhysicalLevel"]==0);
   assert(resetMap->r[resetOffset/4]==0x44000200&&gpio.opens==opens+1);
   assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==0);
   assert(owner.numbers["Cirrus_SPI_GPIO_Group_Unique_Pins"]==1);
   assert(resetMap->r[resetOffset/4]==0x44000200);
   assert(selects.writeGPIOGroup(false)==0&&selects.writeGPIOGroup(true)==0);
   assert(resetMap->r[resetOffset/4]==0x44000201&&csMap->r[csOffset/4]==0x84000201);
  }
  assert(!gpio.opened&&csMap->refs==1&&resetMap->refs==1&&gpio.refs==1);
  // Failure to map a second community preserves the root failure and CS lease.
  gpio.maps.erase(3);
  {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
   assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="COMMUNITY_MAP_FAILED");
   assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==kIOReturnNoResources);
   assert(selects.writeGPIOGroup(false)==kIOReturnNotReady);
  }
  gpio.maps={{4,csMap},{3,resetMap}};resetMap->r[0x114/4]=1;
  {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
   assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="PAD_TX_LOCKED");
   assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==kIOReturnNotPermitted);
   assert(selects.writeGPIOGroup(false)==kIOReturnNotReady&&resetMap->r[resetOffset/4]==0x44000201);
  }
  assert(!gpio.opened&&csMap->refs==1&&resetMap->refs==1&&gpio.refs==1);
  gpio.maps.clear();gpio.values["_HID"]=make<OSString>("INTC1056");
 }
 amp.values.erase("_DSD");amp.values["_CRS"]=make<OSData>(crs);
 // Validation failures must not be reclassified as an absent/native-only DSD.
 spi.dsdValidation=kIOReturnUnsupported;const auto opensBefore=gpio.opens;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="DSD_VALIDATION_FAILED");}
 spi.dsdValidation=0;spi.dsdEvaluation=kIOReturnUnsupported;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported&&selects.count()==0);
  assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Reason"]=="DSD_REFERENCE_CONVERSION_FAILED");
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
 // A retained package may be malformed after validation; lookup must fail closed.
 {ACPIDeviceProperties props;assert(props.load(&spi)==kIOReturnSuccess);
  assert(props.get(nullptr)==nullptr);
  auto* retained=OSDynamicCast(OSArray,dsd->getObject(1));
  auto* saved=retained->v[0];retained->v[0]=nullptr;
  assert(props.get("cs-gpios")==nullptr);retained->v[0]=saved;}
 // Captured macOS behavior: _DSD evaluates successfully but returns NULL.
 // Interpreter references preserve the exact live namespace.
 auto* bridge=properties(array({number(0),make<OSString>("\\_SB.PC00.SPI0.GSPK"),number(0),number(0),number(0)}));
 auto* bridgeEntries=OSDynamicCast(OSArray,bridge->getObject(1));
 spi.dsdNullResult=true;spi.values["INTERPRETER_DSD"]=bridge;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0&&selects.select(1));}
 assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Source"]=="_DSD_INTERPRETER");
 gpio.values["_HID"]=make<OSString>("INT3450");
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 gpio.values["_HID"]=make<OSString>("INTC1056");
 spi.path="IOACPIPlane:/_SB/NEW/SPI8";amp.path=spi.path+"/AMPS";
 auto* cs=OSDynamicCast(OSArray,OSDynamicCast(OSArray,bridgeEntries->getObject(0))->getObject(1));
 cs->v[1]=make<OSString>("\\_SB.NEW.SPI8.AMPS");
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 spi.path="IOACPIPlane:/_SB/PC00/SPI0";amp.path=spi.path+"/GSPK";
 cs->v[1]=make<OSString>("\\_SB.PC00.SPI0.GSPK");
 spi.dsdEvaluation=kIOReturnUnsupported;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==0);}
 spi.dsdEvaluation=0;
 // Returned malformed native objects must not be hidden by raw evaluation.
 spi.dsdNullResult=false;spi.values["_DSD"]=invalidRoot;
 {IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner)==kIOReturnUnsupported);}
 spi.values["_DSD"]=dsd;spi.values.erase("INTERPRETER_DSD");
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
 // GU605MV DSDT topology: CS1 on GPP_E_2; reset and speaker ID in community 5.
 {
  IOACPIPlatformDevice mtlSpi,mtlAmp,mtlGpio;IOService mtlOwner;
  mtlSpi.path="IOACPIPlane:/_SB/PC00/SPI0";mtlAmp.path=mtlSpi.path+"/SPK1";mtlGpio.path="IOACPIPlane:/_SB/GPI0";
  devices={&mtlSpi,&mtlAmp,&mtlGpio};
  mtlGpio.values["_HID"]=make<OSString>("INTC1083");
  auto* csMap=make<IOMemoryMap>();auto* otherMap=make<IOMemoryMap>();
  mtlGpio.maps={{1,csMap},{4,otherMap}};
  csMap->r[0]=otherMap->r[0]=0x940000;csMap->r[3]=otherMap->r[3]=0x700;
  const unsigned csOffset=0x700+27*16,resetOffset=0x700+26*16,inputOffset=0x700+8*16;
  csMap->r[csOffset/4]=0x84000201;otherMap->r[resetOffset/4]=0x44000200;otherMap->r[inputOffset/4]=0x84000100;
  auto descriptor=[&](unsigned pin,bool input){
   auto d=crs;d.resize(d.size()-2);d[23]=pin;d[24]=pin>>8;d[7]=input?1:2;return d;
  };
  auto resources=descriptor(0x82,false);
  for(auto d:{descriptor(0x181,false),descriptor(0x168,true)})resources.insert(resources.end(),d.begin(),d.end());
  resources.insert(resources.end(),{0x79,0});mtlAmp.values["_CRS"]=make<OSData>(resources);
  mtlSpi.values["_DSD"]=properties(array({number(0),&mtlAmp,number(0),number(0),number(0)}));
  auto* metadata=properties(array({number(0)}));auto* entries=OSDynamicCast(OSArray,metadata->getObject(1));
  entries->v.push_back(array({make<OSString>("reset-gpios"),array({&mtlAmp,number(1),number(0),number(0),&mtlAmp,number(1),number(0),number(0)})}));
  entries->v.push_back(array({make<OSString>("spk-id-gpios"),array({&mtlAmp,number(2),number(0),number(0),&mtlAmp,number(2),number(0),number(0)})}));
  mtlAmp.values["_DSD"]=metadata;
  std::vector<uint32_t> before(otherMap->r,otherMap->r+0x10000/4);
  {IntelSPIChipSelects selects;assert(selects.bind(&mtlSpi,&mtlOwner,nullptr,true)==0);
   assert(selects.select(0)==nullptr&&selects.select(1));
   assert(mtlOwner.numbers["Cirrus_SPI_Reset_0_Memory_Index"]==4&&mtlOwner.numbers["Cirrus_SPI_Reset_0_PhysicalLevel"]==0);
   uint32_t speaker=99;assert(selects.readGPIO(&mtlAmp,"spk-id-gpios",0,speaker)==0&&speaker==0);
   assert(selects.readGPIO(&mtlAmp,"spk-id-gpios",1,speaker)==0&&speaker==0);
   assert(mtlOwner.numbers["Cirrus_GPIO_Input_Memory_Index"]==4);
   assert(selects.select(1)->setActive(true)==0&&selects.select(1)->setActive(false)==0);
   assert(std::vector<uint32_t>(otherMap->r,otherMap->r+0x10000/4)==before);
  }
  assert(!mtlGpio.opened&&mtlGpio.opens==1&&mtlGpio.closes==1&&csMap->refs==1&&otherMap->refs==1);
 }
 // Dell Horizon 17 / Alienware x17 R2 (Issue #12):
 // INTC1055 GPIO controller, CS1 on pin 304 (Community 2), Reset on pin 103 (Community 1).
 // Neither controller nor amplifier nor GPIO controller has native _DSD; all return
 // kIOReturnLockedWrite (0xE00002C4). The stack resolves CS and reset via Cirrus_Board_Metadata.
 {
  IOACPIPlatformDevice tglSpi,tglAmp,tglGpio;IOService tglOwner;
  tglSpi.path="IOACPIPlane:/_SB/PC00/SPI2";tglAmp.path=tglSpi.path+"/SPKR";tglGpio.path="IOACPIPlane:/_SB/GPI0";
  devices={&tglSpi,&tglAmp,&tglGpio};
  tglSpi.dsdValidation=kIOReturnLockedWrite;tglAmp.dsdValidation=kIOReturnLockedWrite;
  tglGpio.values["_HID"]=make<OSString>("INTC1055");
  tglGpio.dsdValidation=kIOReturnLockedWrite;
  auto* comm1Map=make<IOMemoryMap>();auto* comm2Map=make<IOMemoryMap>();
  tglGpio.maps={{1,comm1Map},{2,comm2Map}};
  comm1Map->r[0]=comm2Map->r[0]=0x920000;comm1Map->r[3]=comm2Map->r[3]=0x700;
  const unsigned csOffset=0x700+40*16;
  const unsigned resetOffset=0x700+7*16;
  comm2Map->r[csOffset/4]=0x84000201;
  comm1Map->r[resetOffset/4]=0x44000200;
  auto descriptor=[&](unsigned pin,bool input){
   auto d=crs;d.resize(d.size()-2);d[23]=pin;d[24]=pin>>8;d[7]=input?1:2;return d;
  };
  auto resources=descriptor(304,false);
  for(auto d:{descriptor(103,false)})resources.insert(resources.end(),d.begin(),d.end());
  resources.insert(resources.end(),{0x79,0});tglAmp.values["_CRS"]=make<OSData>(resources);
  cirrus::support::ACPIBoardMetadata metadata;
  auto* spiNode=metadata.node(&tglSpi);auto* ampNode=metadata.node(&tglAmp);
  assert(metadata.references(spiNode,"cs-gpios",&tglAmp,0,0,false,1,1));
  assert(metadata.references(ampNode,"reset-gpios",&tglAmp,1,0,false,2));
  assert(metadata.publish(&tglOwner));
  {IntelSPIChipSelects selects;assert(selects.bind(&tglSpi,&tglOwner,nullptr,true)==0);
   assert(tglOwner.strings["Cirrus_Intel_SPI_CS_DSD_Source"]=="BOARD_TABLE");
   assert(tglOwner.numbers["Cirrus_Intel_SPI_CS_DSD_Validate_Return"]==static_cast<uint32_t>(kIOReturnLockedWrite));
   assert(tglOwner.numbers["Cirrus_SPI_GPIO_DSD_Validate_Return"]==static_cast<uint32_t>(kIOReturnLockedWrite));
   assert(selects.select(0)==nullptr&&selects.select(1));
   assert(tglOwner.strings["Cirrus_SPI_Reset_0_Status"]=="OBSERVED_WITHOUT_WRITES");
   assert(tglOwner.numbers["Cirrus_SPI_Reset_0_Memory_Index"]==1&&tglOwner.numbers["Cirrus_SPI_Reset_0_PhysicalLevel"]==0);
   assert(selects.select(1)->setActive(true)==0&&selects.select(1)->setActive(false)==0);
   assert(selects.prepareGPIOGroup(&tglAmp,"reset-gpios")==0&&tglOwner.numbers["Cirrus_SPI_GPIO_Group_Unique_Pins"]==1);
   assert(selects.writeGPIOGroup(false)==0&&selects.writeGPIOGroup(true)==0);
   assert(comm1Map->r[resetOffset/4]==0x44000201);
  }
  assert(!tglGpio.opened&&tglGpio.opens==1&&tglGpio.closes==1&&comm1Map->refs==1&&comm2Map->refs==1);
 }
}
''')
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS production SPI GPIO binding: _DSD references, _CRS, provider exclusivity, maps, PM, failure cleanup")
