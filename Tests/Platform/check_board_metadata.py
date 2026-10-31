"""Replay stock issue #5 resources with no injected _DSD/CSDP/SSDT."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
import re
from acpi_kernel_fixture import install
from host_build import sanitizer_flags

ROOT = Path(__file__).resolve().parents[2]
# Verified Linux generic_dsd_config table, including every GPIO resource
# index, channel, boost type and electrical value. No network dependency in CI.
table = (ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Board/BoardProperties.hpp").read_text()
match_body = re.search(r"inline constexpr BoardProperties kBoardProperties\[\] = \{(.*?)\};", table, re.S)
assert match_body, "Could not find kBoardProperties array in BoardProperties.hpp"
lines = [line.strip().rstrip(",") for line in match_body.group(1).splitlines() if line.strip() and not line.strip().startswith("//")]
assert len(lines) > 0, "No board properties found"

row_pattern = re.compile(
    r"^\{0x([0-9a-fA-F]{8}),\s*([24]),\s*(true|false),\s*\{([01](?:,\s*[01]){3})\},\s*"
    r"(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}$"
)
ssids = set()
for line in lines:
    match = row_pattern.match(line)
    assert match, f"Malformed board row: {line}"
    ssid_str, count_str, boost_str, channels_str, reset_str, spk_str, cs_str, ind_str, peak_str, cap_str = match.groups()
    ssid = int(ssid_str, 16)
    assert (ssid >> 16) != 0 and (ssid & 0xFFFF) != 0, f"Invalid vendor or device in SSID: {ssid_str}"
    assert ssid not in ssids, f"Duplicate subsystem ID: {ssid_str}"
    ssids.add(ssid)
    count = int(count_str)
    channels = [int(c.strip()) for c in channels_str.split(",")]
    if count == 2:
        assert channels[2] == 0 and channels[3] == 0, f"Count is 2 but higher channel indices are non-zero: {line}"
    reset_res, spk_res, cs_res = int(reset_str), int(spk_str), int(cs_str)
    assert reset_res >= -1 and spk_res >= -1 and cs_res >= -1, f"Resource indices must be >= -1: {line}"
    internal_boost = (boost_str == "true")
    ind, peak, cap = int(ind_str), int(peak_str), int(cap_str)
    if internal_boost:
        assert ind > 0 and peak > 0 and cap > 0, f"Internal boost must have non-zero electrical values: {line}"
    else:
        assert ind == 0 and peak == 0 and cap == 0, f"External boost must have zero electrical values: {line}"
with tempfile.TemporaryDirectory(prefix="cirrus-board-metadata-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include <algorithm>
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
struct Reader:platform::BoardDiscoveryContext::GPIOAccess {
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
namespace cirrus::transport {
ACPIPropertyEvaluator nativePropertyEvaluator{nullptr};
const char* nativePropertySetupState{"CALLBACK_NOT_REACHED"};
}
int main(){
 assert(!boardProperties(0x1043,0xffff));
 for(const auto& board:kBoardProperties){
  assert(board.count==2||board.count==4);
  for(unsigned i=0;i<board.count;++i)assert(board.channels[i]<=1);
  assert(board.resetResource>=-1&&board.speakerResource>=-1&&board.csResource>=-1);
  if(board.internalBoost)assert(board.inductor&&board.peak&&board.capacitor);
  else assert(!board.inductor&&!board.peak&&!board.capacitor);
 }
 for(bool renamed:{false,true})for(bool reversed:{false,true}){
  IOACPIPlatformDevice spi,amp,gpio;IOService owner;
  spi.path=renamed?"IOACPIPlane:/_SB/TEST/SPI7":"IOACPIPlane:/_SB/PC00/SPI3";
  amp.path=spi.path+(renamed?"/AMPS":"/SPK1");gpio.path="IOACPIPlane:/_SB/GPI0";
  ::devices={&spi,&amp,&gpio};
  auto crs=capturedCRS();
  if(reversed){const size_t descriptorBytes=3+crs[1]+(size_t(crs[2])<<8);
   std::rotate(crs.begin(),crs.begin()+descriptorBytes,crs.begin()+2*descriptorBytes);}
  if(renamed){const std::string old="PC00.SPI3",replacement="TEST.SPI7";
   for(size_t i=0;i+old.size()<=crs.size();++i)
    if(!memcmp(crs.data()+i,old.data(),old.size()))memcpy(crs.data()+i,replacement.data(),replacement.size());}
  const platform::BoardIdentity identity{0x1043,0x1c9f,"CSC3551"};
  spi.dsdValidation=amp.dsdValidation=gpio.dsdValidation=kIOReturnError;
  spi.absentValidation=amp.absentValidation=gpio.absentValidation=kIOReturnError;
  for(uint8_t selection:{crs[19],uint8_t(2)}){
   auto invalid=crs;const size_t descriptorBytes=3+crs[1]+(size_t(crs[2])<<8);
   invalid[descriptorBytes+19]=selection;
   amp.values["_CRS"]=make<OSData>(invalid);IOService rejected;
   assert(!prepareBoardMetadata(identity,&amp,&rejected)&&!rejected.getProperty("Cirrus_Board_Metadata"));
  }
  amp.values["_CRS"]=make<OSData>(crs);
  gpio.values["_HID"]=make<OSString>("INTC1085");
  auto* cs=make<IOMemoryMap>();auto* reset=make<IOMemoryMap>();auto* speaker=make<IOMemoryMap>();
  for(auto* map:{cs,reset,speaker}){map->r[0]=0x960000;map->r[3]=0x700;}
  cs->r[(0x700+20*16)/4]=0x84000201;reset->r[0x700/4]=0x44000200;
  const auto input=(0x700+24*16)/4;speaker->r[input]=0x84000102;
  gpio.maps={{4,cs},{3,reset},{1,speaker}};
  assert(prepareBoardMetadata(identity,&amp,&owner));
  assert(owner.strings["Cirrus_Board_Metadata_Source"]=="CS35L41_VERIFIED_BOARD_TABLE");
  // Metadata does not mutate firmware/provider devices or publish AML methods.
  assert(amp.values.size()==1&&spi.values.empty()&&gpio.values.size()==1);
  {transport::ACPIDeviceProperties p;assert(p.load(&amp,&owner)==0);
   assert(p.load(&amp,&owner)==kIOReturnBadArgument&&!p.get("cirrus,dev-index"));}
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
  {IOService rejected;assert(!prepareBoardMetadata(identity,&amp,&rejected));
   assert(!rejected.getProperty("Cirrus_Board_Metadata"));}
  amp.dsdEvaluation=kIOReturnIOError;
  {IOService rejected;assert(!prepareBoardMetadata(identity,&amp,&rejected));}
  amp.dsdEvaluation=0;
  // As in Linux, existing native device properties prevent installation of
  // additional driver GPIO mappings. Board electrical facts remain available.
  amp.values["_DSD"]=array({make<OSData>(std::vector<uint8_t>{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01}),
                           array({array({make<OSString>("cirrus,dev-index"),array({number(0),number(1)})})})});
  {IOService nativeOwner;assert(prepareBoardMetadata(identity,&amp,&nativeOwner));
   transport::ACPIDeviceProperties p;assert(p.load(&amp,&nativeOwner)==0&&p.hasNativeProperties());
   assert(!p.get("reset-gpios")&&!p.get("speaker-id-gpios"));
   transport::ACPIDeviceProperties controller;assert(controller.load(&spi,&nativeOwner)==0&&!controller.get("cs-gpios"));}
  amp.values.erase("_DSD");amp.dsdValidation=kIOReturnError;
  IOService unknown;assert(prepareBoardMetadata({0x1043,0xffff,"CSC3551"},&amp,&unknown));
  assert(!unknown.getProperty("Cirrus_Board_Metadata"));
  IOService unsupported;
  assert(!prepareBoardMetadata({0x1043,0x1683,"CSC3551"},&amp,&unsupported));
  assert(!unsupported.getProperty("Cirrus_Board_Metadata"));
  assert(!prepareBoardMetadata({0x17aa,0x38c7,"CSC3551"},&amp,&unsupported));
  amp.dsdValidation=0;amp.values["_DSD"]=make<OSNumber>(0);amp.dsdNullResult=true;
  IOService hpOwner;const platform::BoardIdentity hpIdentity{0x103c,0x89c6,"CSC3551"};
  assert(prepareBoardMetadata(hpIdentity,&amp,&hpOwner));
  assert(hpOwner.strings["Cirrus_Board_Metadata_Source"]=="CS35L41_VERIFIED_BOARD_TABLE");
  {transport::ACPIDeviceProperties hpProps;assert(hpProps.load(&amp,&hpOwner)==0);
   assert(!strcmp(hpProps.source(),"BOARD_TABLE"));
   assert(!strcmp(hpProps.reason(),"BOARD_METADATA_READY"));
   auto* positions=OSDynamicCast(OSArray,hpProps.get("cirrus,speaker-position"));
   assert(positions&&positions->getCount()==2);
   auto* pos0=OSDynamicCast(OSNumber,positions->getObject(0));
   auto* pos1=OSDynamicCast(OSNumber,positions->getObject(1));
   assert(pos0&&pos0->unsigned32BitValue()==1&&pos1&&pos1->unsigned32BitValue()==0);
   auto* resetGpios=OSDynamicCast(OSArray,hpProps.get("reset-gpios"));
   assert(resetGpios&&resetGpios->getCount()==8);
   auto* resetRes=OSDynamicCast(OSNumber,resetGpios->getObject(1));
   assert(resetRes&&resetRes->unsigned32BitValue()==1);
   assert(!hpProps.get("speaker-id-gpios"));}
  {transport::ACPIDeviceProperties spiProps;assert(spiProps.load(&spi,&hpOwner)==0);
   auto* csGpios=OSDynamicCast(OSArray,spiProps.get("cs-gpios"));
   assert(csGpios&&csGpios->getCount()==5);
   auto* csPrefix=OSDynamicCast(OSNumber,csGpios->getObject(0));
   assert(csPrefix&&csPrefix->unsigned32BitValue()==0);
   auto* csRes=OSDynamicCast(OSNumber,csGpios->getObject(2));
   assert(csRes&&csRes->unsigned32BitValue()==0);}
  amp.dsdNullResult=false;amp.values.erase("_DSD");amp.dsdValidation=kIOReturnError;
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
