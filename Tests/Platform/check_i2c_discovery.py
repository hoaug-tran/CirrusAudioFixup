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
int main(){
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
  platform::BoardDiscoveryContext::SpeakerOverrides ids{{SPEAKER_LEFT,SPEAKER_RIGHT},{true,true}};
  const platform::BoardDiscoveryContext context{{VENDOR,DEVICE,"CSC3551"},resources.endpoints,resources.count,properties,nullptr,&ids};
  auto* board=resolveBoardConfiguration(context,assets,reason);assert(board&&board->configuration.count==2);
  for(size_t i=0;i<2;++i){
   const auto& binding=board->configuration.amplifiers[i];DeviceConfiguration config;
   assert(binding.endpoint.i2cAddress==FIRST_ADDRESS+i&&binding.speakerId==ids.values[i]);
   assert(!strcmp(binding.endpoint.controllerPath,nub.path.c_str()));
   assert(decodeDeviceConfiguration(binding.driverConfiguration,binding.driverConfigurationSize,binding.driverConfigurationKind,config)&&config.power.switched());
  }
  delete board;
  ids.known[0]=ids.known[1]=false;assert(!resolveBoardConfiguration(context,assets,reason)&&!strcmp(reason,"CS35L41_SPEAKER_ID_REQUIRED"));
 }
 nub.objects["i2cAddress"]=make<OSNumber>(0x23);
 {transport::I2CPlatformResources bad;assert(!bad.load(&nub,reason)&&!strcmp(reason,"I2C_PROVIDER_ENDPOINT_UNMATCHED"));}
 nub.objects.erase("acpi-device");
 {transport::I2CPlatformResources bad;assert(!bad.load(&nub,reason)&&!strcmp(reason,"I2C_ACPI_DEVICE_UNAVAILABLE"));}
 assert(descriptor->refs==1&&amp->refs==1);
}
'''
for name, value in {
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
