"""Replay captured nub identity and ACPI contracts through production discovery.

Descriptors are reconstructed from the supplied SSDT, not claimed as raw _CRS.
Other addresses/counts exercise portability; no physical I2C behavior is faked.
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from acpi_kernel_fixture import install
from host_build import sanitizer_flags
from production_source_fixture import service_source, method

ROOT = Path(__file__).resolve().parents[2]
capture = json.loads((ROOT / "Tests/Fixtures/Captured/ZenbookUM3402YAR.json").read_text())
assert capture["speaker_ids"] == [1, 1] and capture["r0"] == [10344, 10628]
CPP = r'''
#define KERNEL 1
#include "Fixture.hpp"
inline const void* gIOServicePlane=IORegistryEntry::getPlane("IOService");
#include "Platform/ACPI/I2CPlatformResources.hpp"
#include "Devices/Amplifiers/CS35L41/Board/CS35L41BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
using namespace cirrus;
using namespace devices::amplifiers::cs35l41;
std::vector<uint8_t> crs(std::initializer_list<unsigned> addresses){
 std::vector<uint8_t> bytes;
 for(auto address:addresses){
  const char source[]="\\_SB.I2CD";std::vector<uint8_t> d(18+sizeof(source));
  d[0]=0x8e;d[1]=uint8_t(d.size()-3);d[3]=1;d[5]=1;d[6]=2;d[9]=1;d[10]=6;
  const uint32_t speed=CLOCK_HZ;
  for(unsigned i=0;i<4;++i)d[12+i]=uint8_t(speed>>(i*8));
  d[16]=uint8_t(address);d[17]=uint8_t(address>>8);memcpy(d.data()+18,source,sizeof(source));
  bytes.insert(bytes.end(),d.begin(),d.end());
 }
 bytes.push_back(0x79);bytes.push_back(0);return bytes;
}
bool assets(uint16_t v,uint16_t d,uint32_t id){return selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,v,d,id)!=nullptr;}
struct ResetService:IOService {
 bool mPlatformAuthorized=true;IOService* mProvider=nullptr;platform::DiscoveredBoard* mDiscoveredBoard=nullptr;
 struct Native {
  bool bound(){return false;}
  IOReturn prepareHardware(IOACPIPlatformDevice*){assert(false);return -1;}
  struct Failure {const char* reason="SPI";};Failure failure(){return {};}
 } mNativeSPI;
 bool bootArgEnabled(const char*){return false;}
 bool bootArgStrEquals(const char*,const char*){return false;}
 unsigned failures=0;
 void recordAttachmentFailure(const char*,const char*,IOReturn){++failures;}
 bool performPlatformHardwareReset();
};
constexpr bool kOSBooleanFalse=false;
RESET_METHOD
struct ResetWindow {
 uint32_t value=(1U<<28)|(1U<<29)|(1U<<20);
 std::vector<uint32_t> writes;unsigned reads=0,unavailableAt=0;bool rejectAssertion=false,rejectRelease=false;
 uint32_t read32(uint32_t offset){assert(offset==84*4);return ++reads==unavailableAt?UINT32_MAX:value;}
 void write32(uint32_t offset,uint32_t v){assert(offset==84*4);writes.push_back(v);
  if((writes.size()==1&&rejectAssertion)||(writes.size()==2&&rejectRelease))return;
  value=v;
 }
};
int main(){
 for(unsigned fault=0;fault<5;++fault){
  ResetWindow window;window.rejectAssertion=fault==1;window.rejectRelease=fault==2;window.unavailableAt=fault>=3?fault-1:0;
  unsigned delays=0;const char* why=nullptr;
  const bool ok=transport::amd::pulseGPIOReset(window,1024,84,false,[&](uint32_t ms){assert(ms==2);++delays;},why);
  assert(ok==(fault==0)&&delays==2&&window.writes.size()==2);
  assert(!(window.writes[0]&((1U<<28)|(1U<<29)|(1U<<22))));
  assert(window.writes[1]&(1U<<22)); // release is attempted after any assertion failure
 }
 for(auto pin:{63U,190U,191U,256U}){
  ResetWindow window;const char* why=nullptr;
  assert(!transport::amd::pulseGPIOReset(window,1024,uint16_t(pin),false,[](uint32_t){assert(false);},why));
  assert(window.writes.empty()&&!window.reads);
 }

 auto captured=crs({FIRST_ADDRESS,SECOND_ADDRESS});transport::ACPII2CResources parsed;
 assert(transport::parseACPII2CResources(captured.data(),captured.size(),parsed)&&parsed.count==2);
 for(size_t i=0;i<captured.size();++i){auto bad=parsed;assert(!transport::parseACPII2CResources(captured.data(),i,bad));assert(bad.count==2);}
 for(auto addresses:{std::initializer_list<unsigned>{0x22}, {0x31,0x42}, {0x11,0x25,0x37,0x49}}){
  auto bytes=crs(addresses);assert(transport::parseACPII2CResources(bytes.data(),bytes.size(),parsed)&&parsed.count==addresses.size());
  size_t i=0;for(auto address:addresses)assert(parsed.endpoints[i++].address==address);
 }
 for(auto addresses:{std::initializer_list<unsigned>{0x40,0x40}, {0x80}}){auto bytes=crs(addresses);assert(!transport::parseACPII2CResources(bytes.data(),bytes.size(),parsed));}
 for(unsigned offset:{3U,4U,5U,6U,7U,9U,10U,12U}){
  auto bad=captured;bad[offset]=0xff;assert(!transport::parseACPII2CResources(bad.data(),bad.size(),parsed)||offset==12);
 }
 auto* amp=make<IOACPIPlatformDevice>();auto* descriptor=make<OSData>(captured);amp->values["_CRS"]=descriptor;
 amp->dsdValidation=kIOReturnNotFound;
 IOService nub;nub.path="IOService:/AppleACPIPlatformExpert/I2CD/VoodooI2CACPIController/VoodooI2CControllerNub/VoodooI2CControllerDriver/SPKR";
 nub.objects["acpi-device"]=amp;nub.objects["i2cAddress"]=make<OSNumber>(FIRST_ADDRESS);nub.objects["addrWidth"]=make<OSNumber>(7);
 const char* reason=nullptr;
 {
  transport::I2CPlatformResources resources;assert(resources.load(&nub,reason)&&resources.count==2);
  transport::ACPIPlatformProperties properties(resources.properties);
  const platform::BoardDiscoveryContext context{{VENDOR,DEVICE,"CSC3551"},resources.endpoints,resources.count,properties};
  assert(!resolveBoardConfiguration(context,assets,reason)&&!strcmp(reason,"CS35L41_SPEAKER_ID_REQUIRED"));
  platform::DiscoveredBoard* board=nullptr;
 struct GPIO final:platform::BoardDiscoveryContext::GPIOReader {
  unsigned calls=0;
  bool read(const char*,size_t,uint32_t&)override{return false;}
  bool readResource(uint32_t resource,uint32_t pin,bool low,uint32_t& value)override{
   assert(resource==1&&pin==0&&!low);++calls;value=SPEAKER_LEFT;return true;
  }
 } gpio;
 platform::BoardDiscoveryContext autoContext{context.identity,resources.endpoints,resources.count,properties,&gpio};
 board=resolveBoardConfiguration(autoContext,assets,reason);
 assert(board&&gpio.calls==2);
 for(size_t i=0;i<2;++i)assert(board->configuration.amplifiers[i].speakerId==SPEAKER_LEFT);
 delete board;

 }
 // Synthesized GPIO descriptors exercise the real ACPI/MMIO reader. Physical pin
 // and live register values were not captured; they are not presented as a dump.
 assert(transport::amd::supportsGPIOInput("AMD0030")&&transport::amd::supportsGPIOInput("AMDI0030")&&
        transport::amd::supportsGPIOInput("AMDI0031")&&!transport::amd::supportsGPIOInput("AMDI0032"));
 auto gpioDescriptor=[](unsigned pin,unsigned restriction){
  const char source[]="\\_SB.GPIO";std::vector<uint8_t> d(25+sizeof(source));
  d[0]=0x8c;d[1]=uint8_t(d.size()-3);d[3]=1;d[4]=1;d[5]=1;d[7]=restriction;
  d[14]=23;d[17]=25;d[19]=uint8_t(d.size());d[23]=uint8_t(pin);d[24]=uint8_t(pin>>8);
  memcpy(d.data()+25,source,sizeof(source));return d;
 };
 amp->path="IOACPIPlane:/_SB/I2CD/SPKR";
 auto* controller=make<IOACPIPlatformDevice>();controller->path="IOACPIPlane:/_SB/GPIO";
 const char hid[]="AMDI0030";
 controller->objects["name"]=make<OSData>(std::vector<uint8_t>(hid,hid+sizeof(hid)));
 auto* memory=make<IOMemoryMap>();controller->map=memory;memory->bytes=1024;
 ::devices={amp,controller};
 auto full=captured;full.resize(full.size()-2);
 for(auto d:{gpioDescriptor(84,2),gpioDescriptor(SPEAKER_PIN,0x19)})full.insert(full.end(),d.begin(),d.end());
 full.insert(full.end(),{0x79,0});amp->values["_CRS"]=make<OSData>(full);
 {
  transport::I2CPlatformResources automatic;IOService owner;
  automatic.setDiagnosticOwner(&owner);assert(automatic.load(&nub,reason));
  transport::ACPIPlatformProperties native(automatic.properties);
  platform::BoardDiscoveryContext context{{VENDOR,DEVICE,"CSC3551"},automatic.endpoints,automatic.count,native,&automatic};
  memory->r[SPEAKER_PIN]=1U<<16;
  std::vector<uint32_t> before(memory->r,memory->r+sizeof(memory->r)/4);
  auto* board=resolveBoardConfiguration(context,assets,reason);if(!board)fprintf(stderr,"board=%s gpio=%s pin=%llu\n",reason,owner.strings["Cirrus_GPIO_Input_Reason"].c_str(),(unsigned long long)owner.numbers["Cirrus_GPIO_Input_Pin"]);assert(board);
  for(size_t i=0;i<2;++i)assert(board->configuration.amplifiers[i].speakerId==1);
  delete board;
  assert(owner.strings["Cirrus_GPIO_Input_Reason"]=="GPIO_INPUT_READ_WITHOUT_WRITES");
  uint32_t value=7;
  assert(automatic.readResource(1,0,true,value)&&value==0);
  memory->r[SPEAKER_PIN]=0;assert(!resolveBoardConfiguration(context,assets,reason));
  assert(!strcmp(reason,"CS35L41_SPEAKER_TUNING_UNAVAILABLE"));
  for(auto config:{UINT32_MAX,1U<<23}){
   memory->r[SPEAKER_PIN]=config;value=7;
   assert(!automatic.readResource(1,0,false,value)&&value==7);
  }
  memory->r[SPEAKER_PIN]=1U<<16;memory->bytes=16;
  assert(!automatic.readResource(1,0,false,value));memory->bytes=1024;
  assert(!owner.numbers.count("Cirrus_GPIO_Input_Value"));
  controller->values["_PSC"]=make<OSNumber>(3);
  assert(!automatic.readResource(1,0,false,value));controller->values.erase("_PSC");
  auto* identity=controller->objects["name"];controller->objects.erase("name");
  assert(!automatic.readResource(1,0,false,value));controller->objects["name"]=identity;
  assert(!automatic.readResource(2,0,false,value));
  assert(!automatic.readResource(1,1,false,value));
  assert(!memcmp(memory->r,before.data(),sizeof(memory->r)));
  assert(!controller->opens&&!controller->closes);
  // Reproduce the latest capture: identity transfers fail until firmware reset
  // is released. Reset pin84 is synthetic; the observed speaker pin is real.
  memory->r[84]=(1U<<28)|(1U<<29)|(1U<<20);
  unsigned pulses=0;
  fixtureSleep=[&](unsigned ms){
   assert(ms==2);
   assert((memory->r[84]&(1U<<23))&&!(memory->r[84]&((1U<<28)|(1U<<29))));
   assert(bool(memory->r[84]&(1U<<22))==bool(pulses++%2));
  };
  auto identityReturn=[&](){return (memory->r[84]&(1U<<22))?0U:CAPTURED_ERROR;};
  assert(identityReturn()==CAPTURED_ERROR);
  board=resolveBoardConfiguration(context,assets,reason);assert(board);
  ResetService service;service.mProvider=&nub;service.mDiscoveredBoard=board;
  assert(service.performPlatformHardwareReset());assert(pulses==2&&identityReturn()==0);
  assert(service.strings["Cirrus_Reset_Policy"]=="ACPI_GPIO_PREPARED");
  assert(service.strings["Cirrus_I2C_Reset_Reason"]=="RESET_PULSE_COMPLETE");
  assert(memory->r[84]==((1U<<20)|(1U<<23)|(1U<<22)));
  delete board;fixtureSleep={};
  const uint32_t resetHigh=memory->r[84];
  controller->opened=true;assert(!automatic.reset(nullptr,0));controller->opened=false;
  controller->values["_PSC"]=make<OSNumber>(3);assert(!automatic.reset(nullptr,0));controller->values.erase("_PSC");
  assert(memory->r[84]==resetHigh&&controller->opens==1&&controller->closes==1);
  assert(!automatic.reset(nullptr,1)); // speaker input cannot become reset output
  assert(memory->r[SPEAKER_PIN]==(1U<<16));
  before.assign(memory->r,memory->r+sizeof(memory->r)/4);

  // Native _DSD references take precedence over the board-table resource.
  auto* tuples=make<OSArray>(std::vector<OSObject*>{amp,make<OSNumber>(1),make<OSNumber>(0),make<OSNumber>(0),
                                                amp,make<OSNumber>(1),make<OSNumber>(0),make<OSNumber>(0)});
  auto* pair=make<OSArray>(std::vector<OSObject*>{make<OSString>("spk-id-gpios"),tuples});
  auto* entries=make<OSArray>(std::vector<OSObject*>{pair});
  amp->values["_DSD"]=make<OSArray>(std::vector<OSObject*>{make<OSData>(std::vector<uint8_t>{
   0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01}),entries});
  amp->dsdValidation=0;
  transport::I2CPlatformResources described;assert(described.load(&nub,reason));
  assert(described.read("spk-id-gpios",1,value)&&value==1);
  assert(!described.read("spk-id-gpios",2,value));
  auto* resetTuples=make<OSArray>(std::vector<OSObject*>{amp,make<OSNumber>(0),make<OSNumber>(0),make<OSNumber>(0),
                                                    amp,make<OSNumber>(0),make<OSNumber>(0),make<OSNumber>(0)});
  entries->v.push_back(make<OSArray>(std::vector<OSObject*>{make<OSString>("reset-gpios"),resetTuples}));
  transport::I2CPlatformResources declared;declared.setDiagnosticOwner(&owner);assert(declared.load(&nub,reason));
  assert(declared.reset("reset-gpios",-1));
  resetTuples->v[5]=make<OSNumber>(1);
  assert(!declared.reset("reset-gpios",-1)); // reject distinct reset lines before writes
  assert(memory->r[84]==resetHigh);

  tuples->v[3]=make<OSNumber>(2);assert(!described.read("spk-id-gpios",0,value));
  assert(!memcmp(memory->r,before.data(),sizeof(memory->r)));
 }
 assert(memory->refs==1&&controller->refs==1&&amp->refs==1);
 nub.objects["i2cAddress"]=make<OSNumber>(0x23);
 {transport::I2CPlatformResources bad;assert(!bad.load(&nub,reason)&&!strcmp(reason,"I2C_PROVIDER_ENDPOINT_UNMATCHED"));}
 nub.objects.erase("acpi-device");
 {transport::I2CPlatformResources bad;assert(!bad.load(&nub,reason)&&!strcmp(reason,"I2C_ACPI_DEVICE_UNAVAILABLE"));}
 assert(descriptor->refs==1&&amp->refs==1);
}
'''
# Execute the production reset policy through the I2C path; legacy quirk
# implementation is outside this fixture and retains its separate coverage.
reset = method(service_source(), "CirrusAudioFixup", "performPlatformHardwareReset")
reset = reset[:reset.index("    const cirrus::platform::ResetControllerQuirk*")] + "    return true;\n}"
CPP = CPP.replace("RESET_METHOD", reset.replace("CirrusAudioFixup::", "ResetService::"))
for name, value in {
    "CAPTURED_ERROR": capture["followup"]["probe_returns"][0],
    "SPEAKER_PIN": capture["followup"]["speaker_gpio_pin"],
    "CLOCK_HZ": capture["provider"]["clock_hz"], "FIRST_ADDRESS": capture["endpoints"][0],
    "SECOND_ADDRESS": capture["endpoints"][1], "SPEAKER_LEFT": capture["speaker_ids"][0],
    "SPEAKER_RIGHT": capture["speaker_ids"][1], "VENDOR": capture["identity"]["vendor"], "DEVICE": capture["identity"]["device"],
}.items():
    CPP = re.sub(r"\b" + name + r"\b", str(value), CPP)
with tempfile.TemporaryDirectory(prefix="cirrus-i2c-discovery-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(CPP)
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I"+str(tmp), "-I"+str(ROOT/"CirrusAudioFixup"), str(tmp/"test.cpp"),
                        str(ROOT/"CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/FirmwareData.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS captured Zenbook I2C discovery, measured speaker identities, variable endpoints, malformed resources and provider consistency")
