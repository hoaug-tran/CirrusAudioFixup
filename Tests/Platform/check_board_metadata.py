"""Replay stock issue #5 resources with no injected _DSD/CSDP/SSDT."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
from acpi_kernel_fixture import install
from host_build import sanitizer_flags

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-board-metadata-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include "Fixture.hpp"
#include "Devices/Amplifiers/CS35L41/Board/BoardMetadataFallback.hpp"
#include "Devices/Amplifiers/CS35L41/Board/CS35L41BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
#include "Platform/ACPI/ACPIPlatformProperties.hpp"
#include "Transport/SPI/Intel/IntelSPIChipSelects.hpp"
using namespace cirrus;
using namespace devices::amplifiers::cs35l41;
OSArray* array(std::initializer_list<OSObject*> v){return make<OSArray>(std::vector<OSObject*>(v));}
OSObject* number(uint32_t n){return make<OSNumber>(n);}
bool resources(uint16_t vendor,uint16_t device,uint32_t speaker){
 return selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,vendor,device,speaker)!=nullptr;
}
struct Reader:platform::BoardDiscoveryContext::GPIOReader {
 transport::intel::IntelSPIChipSelects& selects;IOACPIPlatformDevice& amp;
 Reader(transport::intel::IntelSPIChipSelects& s,IOACPIPlatformDevice& a):selects(s),amp(a){}
 bool read(const char* property,size_t index,uint32_t& value)override{return selects.readGPIO(&amp,property,index,value)==0;}
};
std::vector<uint8_t> capturedCRS(){
 const char* hex="8e210002000202000001090000093d0008000000005c5f53422e504330302e5350493300"
 "8e210002000202000001090000093d0008000001005c5f53422e504330302e5350493300"
 "8c20000101010002000100000000170000190023000000d4015c5f53422e4750493000"
 "8c2000010101000200020000000017000019002300000040015c5f53422e4750493000"
 "8c20000101010001000100000000170000190023000000c0005c5f53422e4750493000"
 "8c2000010101000900010000640017000019002300000041015c5f53422e4750493000"
 "8c2000010001000d00010000640017000019002300000041015c5f53422e47504930007900";
 std::vector<uint8_t> bytes;
 for(size_t i=0;hex[i];i+=2){unsigned value=0;assert(sscanf(hex+i,"%2x",&value)==1);bytes.push_back(uint8_t(value));}
 return bytes;
}
int main(){
 for(bool renamed:{false,true}){
  IOACPIPlatformDevice spi,amp,gpio;IOService owner;
  spi.path=renamed?"IOACPIPlane:/_SB/TEST/SPI7":"IOACPIPlane:/_SB/PC00/SPI3";
  amp.path=spi.path+(renamed?"/AMPS":"/SPK1");gpio.path="IOACPIPlane:/_SB/GPI0";
  ::devices={&spi,&amp,&gpio};
  auto crs=capturedCRS();
  if(renamed){const std::string old="PC00.SPI3",replacement="TEST.SPI7";
   for(size_t i=0;i+old.size()<=crs.size();++i)
    if(!memcmp(crs.data()+i,old.data(),old.size()))memcpy(crs.data()+i,replacement.data(),replacement.size());}
  amp.values["_CRS"]=make<OSData>(crs);
  spi.dsdValidation=amp.dsdValidation=gpio.dsdValidation=kIOReturnError;
  spi.absentValidation=amp.absentValidation=gpio.absentValidation=kIOReturnError;
  gpio.values["_HID"]=make<OSString>("INTC1085");
  auto* cs=make<IOMemoryMap>();auto* reset=make<IOMemoryMap>();auto* speaker=make<IOMemoryMap>();
  for(auto* map:{cs,reset,speaker}){map->r[0]=0x960000;map->r[3]=0x700;}
  cs->r[(0x700+20*16)/4]=0x84000201;reset->r[0x700/4]=0x44000200;
  const auto input=(0x700+24*16)/4;speaker->r[input]=0x84000102;
  gpio.maps={{4,cs},{3,reset},{1,speaker}};
  const platform::BoardIdentity identity{0x1043,0x1c9f,"CSC3551"};
  assert(prepareBoardMetadata(identity,&amp,&owner));
  assert(owner.strings["Cirrus_Board_Metadata_Source"]=="CS35L41_VERIFIED_BOARD_TABLE");
  // Metadata does not mutate firmware/provider devices or publish AML methods.
  assert(amp.values.size()==1&&spi.values.empty()&&gpio.values.size()==1);
  {
   transport::intel::IntelSPIChipSelects selects;assert(selects.bind(&spi,&owner,nullptr,true)==0);
   assert(selects.count()==2&&!selects.select(0)&&selects.select(1));
   assert(owner.strings["Cirrus_Intel_SPI_CS_DSD_Source"]=="BOARD_TABLE");
   assert(owner.strings["Cirrus_SPI_Reset_0_Status"]=="OBSERVED_WITHOUT_WRITES");
   transport::ACPIDeviceProperties properties;assert(properties.load(&amp,&owner)==0);
   transport::ACPIPlatformProperties values(properties);
   char controller[256]{};assert(support::ACPIBoardMetadata::path(&spi,controller));
   core::BusEndpoint endpoints[]={{core::BusTransportType::SPI,controller,0,false,0,4000000,0},
                                 {core::BusTransportType::SPI,controller,0,false,1,4000000,0}};
   Reader reader(selects,amp);const char* reason=nullptr;
   for(uint32_t physical:{0U,1U}){
    speaker->r[input]=0x84000100|(physical<<1);
    auto* board=resolveBoardConfiguration({identity,endpoints,2,values,&reader},resources,reason);
    assert(board&&board->configuration.permits(identity));
    for(unsigned i=0;i<2;++i)assert(board->configuration.amplifiers[i].speakerId==physical);
    delete board;assert(speaker->r[input]==(0x84000100|(physical<<1))&&reset->r[0x700/4]==0x44000200);
   }
   speaker->r[input]|=1U<<9;
   assert(!resolveBoardConfiguration({identity,endpoints,2,values,&reader},resources,reason));
   assert(!strcmp(reason,"CS35L41_SPEAKER_ID_UNREADABLE"));
   assert(owner.strings["Cirrus_GPIO_Input_Reason"]=="PAD_NOT_GPIO_INPUT");
   speaker->r[input]&=~(1U<<9);
   // Native data wins, but a supplied identity must match the observed input.
   auto* nativeEntries=array({array({make<OSString>("cirrus,speaker-id"),array({number(0),number(0)})}),
       array({make<OSString>("cirrus,boost-peak-milliamp"),array({number(4100),number(4200)})})});
   auto* nativePackage=array({make<OSData>(std::vector<uint8_t>{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01}),nativeEntries});
   amp.dsdValidation=0;amp.values["_DSD"]=nativePackage;
   {transport::ACPIDeviceProperties p;assert(p.load(&amp,&owner)==0);
    transport::ACPIPlatformProperties source(p);uint32_t peak[2]{};
    assert(source.array("cirrus,boost-peak-milliamp",peak,2)&&peak[0]==4100&&peak[1]==4200);
    assert(!resolveBoardConfiguration({identity,endpoints,2,source,&reader},resources,reason));
    assert(!strcmp(reason,"CS35L41_SPEAKER_ID_MISMATCH"));}
   amp.values.erase("_DSD");amp.dsdValidation=kIOReturnError;
   assert(selects.prepareGPIOGroup(&amp,"reset-gpios")==0&&owner.numbers["Cirrus_SPI_GPIO_Group_Unique_Pins"]==1);
   assert(reset->r[0x700/4]==0x44000200);
   assert(selects.writeGPIOGroup(false)==0&&selects.writeGPIOGroup(true)==0&&reset->r[0x700/4]==0x44000201);
  }
  assert(!gpio.opened&&gpio.opens==1&&gpio.closes==1&&cs->refs==1&&reset->refs==1&&speaker->refs==1);
  // Known-board fallback must not hide native malformed data.
  amp.dsdValidation=0;amp.values["_DSD"]=make<OSData>(std::vector<uint8_t>{1});
  {transport::ACPIDeviceProperties p;assert(p.load(&amp,&owner)==kIOReturnUnsupported);}
  amp.values.erase("_DSD");amp.dsdValidation=kIOReturnError;
  // A returned adapter without its required version is malformed, not absent.
  amp.values["CSDP"]=array({make<OSData>(std::vector<uint8_t>{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01}),array({})});
  {transport::ACPIDeviceProperties p;assert(p.load(&amp,&owner)==kIOReturnUnsupported);
   assert(!strcmp(p.reason(),"ADAPTER_VERSION_UNSUPPORTED"));}
  amp.values.erase("CSDP");
  IOService unknown;assert(prepareBoardMetadata({0x1043,0x14d3,"CSC3551"},&amp,&unknown));
  assert(!unknown.getProperty("Cirrus_Board_Metadata"));
 }
}
''')
    for flags in sanitizer_flags():
        executable = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), str(ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/FirmwareData.cpp"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print("PASS stock ACPI without SSDT: module metadata, dynamic paths, cross-community CS/reset/input, both physical speaker IDs and fail-closed")
