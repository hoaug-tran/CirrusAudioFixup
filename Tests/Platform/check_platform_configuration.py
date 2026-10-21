"""Driver-owned ACPI configuration, legacy identity isolation and asset provenance."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from production_source_fixture import service_source
import hashlib
import re
import subprocess
import tempfile
from production_source_fixture import method

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/Generated/FirmwareData.inc").read_text()
for symbol, size, digest in (
    ("cs35l41_dsp1_spk_prot_103c89c6_wmfw", 34068, "c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4"),
    ("cs35l41_dsp1_spk_prot_103c89c6_spkid0_l0_bin", 4000, "7b6a432787c14a9ba1ec419672c9ff87d0101669ecda37dc14c8e902ed363c42"),
    ("cs35l41_dsp1_spk_prot_103c89c6_spkid0_r0_bin", 4000, "2f7e769606a77ef433e5c1ca35bb4e5679b2d01326aefd9fdc11cdfcb151d81d"),
    ("cs35l41_dsp1_spk_prot_10431c9f_wmfw", 34068, "c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4"),
    ("cs35l41_dsp1_spk_prot_10431c9f_spkid0_l0_bin", 4660, "e7bc9975b5165ab9772ec1e1477b383fb5857198feb35d6d41d0497ce860ae6a"),
    ("cs35l41_dsp1_spk_prot_10431c9f_spkid0_r0_bin", 4656, "1ef25251ff164dfd74ddb41e5f9214e81c464592d60432687de7c21a08abf834"),
    ("cs35l41_dsp1_spk_prot_10431c9f_spkid1_l0_bin", 4648, "f71b292f33ef49e20308eb3d59523988f55c7acf50902bed97f4b07519d9a5cc"),
    ("cs35l41_dsp1_spk_prot_10431c9f_spkid1_r0_bin", 4660, "8c235171e36bd27a0bd31a4d8f30887cbe682e485eeb95b0678bf5381111828e"),
):
    found = re.findall(r"const uint8_t " + symbol + r"\[\] = \{(.*?)\};", header, re.S)
    assert len(found) == 1, symbol
    payload = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", found[0]))
    assert len(payload) == size and hashlib.sha256(payload).hexdigest() == digest, symbol

with tempfile.TemporaryDirectory(prefix="cirrus-hp-profile-") as directory:
    tmp = Path(directory)
    fixture = r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>
#include <algorithm>
#include "Devices/Amplifiers/CS35L41/Board/CS35L41BoardDiscovery.hpp"
#include "Platform/Board/PlatformProfile.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
struct Properties:cirrus::platform::PlatformProperties {
 bool contains(const char* name)const override{return values.count(name)!=0;}
 std::map<std::string,std::vector<uint32_t>> values{
 {"cirrus,dev-index",{0,1}}, {"cirrus,speaker-position",{1,0}},
 {"cirrus,boost-type",{0,0}}, {"cirrus,gpio1-func",{0,0}}, {"cirrus,gpio2-func",{2,2}},
 {"cirrus,boost-ind-nanohenry",{1000,1000}}, {"cirrus,boost-peak-milliamp",{4500,4500}},
 {"cirrus,boost-cap-microfarad",{24,24}}};
 bool array(const char* name,uint32_t* output,size_t count)const override {
  auto i=values.find(name);if(i==values.end()||i->second.size()!=count)return false;
  std::copy(i->second.begin(),i->second.end(),output);return true;
 }
};
bool allowResources=true;
bool assets(uint16_t,uint16_t,uint32_t speaker){return allowResources&&speaker==UINT32_MAX;}
using namespace cirrus;
using namespace devices::amplifiers::cs35l41;
platform::DiscoveredBoard* resolve(Properties& p,platform::BoardIdentity identity={0x103c,0x89c6,"CSC3551"}){
 core::BusEndpoint endpoints[]={{core::BusTransportType::SPI,"\\_SB.PCI0.SPI9",0,false,1,2000000,0},
                               {core::BusTransportType::SPI,"\\_SB.PCI0.SPI9",0,false,0,4000000,0}};
 const char* reason=nullptr;
 return resolveBoardConfiguration({identity,endpoints,2,p},assets,reason);
}
void profileChecks(){
 for(uint32_t speaker:{0U,1U}){
  const auto* resource=selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,0x1043,0x1c9f,speaker);
  assert(resource&&!resource->genericSpeaker&&resource->spkid==speaker);
  size_t left=0,right=0;
  assert(selectChannelCoefficients(*resource,false,left)&&selectChannelCoefficients(*resource,true,right));
  assert(left>0&&right>0&&resource->bin!=resource->binRight);
 }
 for(uint32_t unknown:{2U,UINT32_MAX})
  assert(!selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,0x1043,0x1c9f,unknown));
 assert(!selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,0x1043,0x14d3,1));
 assert(platform::defaultPlatformProfile().amplifierModel==core::AmplifierModel::Unknown);
 assert(!platform::defaultPlatformProfile().endpoints&&!platform::defaultPlatformProfile().endpointCount);
 uint32_t identity=7;
 assert(platform::parseACPISubsystemId("103C89C6",identity)&&identity==0x103c89c6);
 for(auto bad:{"", "103C89C", "0x103C89C6", "103G89C6", "000089C6", "FFFFFFFF"})
  assert(!platform::parseACPISubsystemId(bad,identity)&&identity==0x103c89c6);
 assert(!platform::findPlatformProfile("CSC3551",identity)->allowAutomaticInitialization);
 Properties properties;auto* discovered=resolve(properties);assert(discovered);
 const auto& board=discovered->configuration;assert(board.permits({0x103c,0x89c6,"CSC3551"}));
 assert(board.count==2&&board.useBoardFirmwareIdentity);
 assert(board.amplifiers[0].channelIndex==0&&board.amplifiers[1].channelIndex==1);
 assert(board.amplifiers[0].endpoint.spiChipSelect==1&&board.amplifiers[0].endpoint.spiClockHz==2000000);
 assert(!strcmp(board.amplifiers[0].endpoint.controllerPath,"\\_SB.PCI0.SPI9"));
 assert(!strcmp(board.amplifiers[0].tuningResource,"speaker-protection"));
 DeviceConfiguration configuration;
 const auto& binding=board.amplifiers[0];
 assert(decodeDeviceConfiguration(binding.driverConfiguration,binding.driverConfigurationSize,binding.driverConfigurationKind,configuration));
 assert(configuration.power.internal()&&configuration.power.peakMilliamp==4500);
 delete discovered;
 // Synthetic fixtures prove the resolver is independent of manufacturer and
 // board name. They do not register resources or claim physical OEM support.
 for(auto vendor:{0x103c,0x17aa,0x1234}){
  Properties p;p.values["cirrus,boost-peak-milliamp"]={4100,4000};
  discovered=resolve(p,{uint16_t(vendor),0x5678,"CSC3551"});assert(discovered);
  auto* config=static_cast<const DeviceConfiguration*>(discovered->configuration.amplifiers[0].driverConfiguration);
  assert(config->power.peakMilliamp==4000);delete discovered;
 }
 for(auto key:{"cirrus,dev-index","cirrus,speaker-position","cirrus,boost-type","cirrus,gpio1-func", "cirrus,gpio2-func",
               "cirrus,boost-ind-nanohenry","cirrus,boost-peak-milliamp","cirrus,boost-cap-microfarad"}){
  Properties p;p.values.erase(key);assert(!resolve(p));
 }
 for(auto key:{"cirrus,dev-index","cirrus,speaker-position"}){Properties p;p.values[key]={0,0};assert(!resolve(p));}
 {Properties p;p.values["cirrus,gpio1-func"]={1,0};assert(!resolve(p));}
 {Properties p;p.values["cirrus,gpio2-func"]={3,2};assert(!resolve(p));}
 {Properties p;p.values["cirrus,boost-type"]={1,1};assert(!resolve(p,{0x1234,0x5678,"CSC3551"}));}
 {Properties p;p.values["cirrus,boost-type"]={1,1};auto* corrected=resolve(p);assert(corrected);delete corrected;}
 allowResources=false;assert(!resolve(properties));allowResources=true;
 assert(!resolve(properties,{0,0x89c6,"CSC3551"}));
 assert(platform::findPlatformProfile("CLSA0100")->resetQuirk);
}
'''
    mocks = r'''
struct OSObject {unsigned refs=1;virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs>1);--refs;}};
struct OSString:OSObject {std::string value;explicit OSString(const char* s):value(s){}const char* getCStringNoCopy(){return value.c_str();}};
#define OSDynamicCast(t,o) dynamic_cast<t*>(o)
#define OSSafeReleaseNULL(o) do{if(o){o->release();o=nullptr;}}while(0)
constexpr int kIOReturnSuccess=0;
constexpr bool kOSBooleanTrue=true,kOSBooleanFalse=false;
constexpr unsigned kIOPCIConfigSubSystemVendorID=0x2c,kIOPCIConfigSubSystemID=0x2e;
struct IORegistryEntry:OSObject {};
class IOService:public IORegistryEntry {public:OSObject* getProperty(const char*){return nullptr;}const char* getName(){return "unrecognized";}};
class IOACPIPlatformDevice:public IOService {public:OSString hid{"CSC3551"},sub{"103C89C6"};
 int evaluateObject(const char* name,OSObject** out){*out=!strcmp(name,"_HID")?&hid:!strcmp(name,"_SUB")?&sub:nullptr;if(!*out)return -1;(*out)->retain();return 0;}};
class IOPCIDevice:public IOService {public:uint32_t ssid=0x103c8ca4;uint16_t configRead16(unsigned offset){return offset==0x2c?ssid>>16:ssid&0xffff;}};
#include "Support/KernelDeviceCast.hpp"
#define CIRRUS_LOG(...) ((void)0)
class CirrusAudioFixup {
public:
 IOService* mProvider=nullptr;IOPCIDevice* audio=nullptr;
 const cirrus::platform::PlatformProfile* mPlatformProfile=nullptr;
 cirrus::platform::DiscoveredBoard* mDiscoveredBoard=nullptr;
 cirrus::platform::PlatformProfile mDiscoveredProfile{};
 uint32_t mPlatformSubsystemId=0;bool mPlatformAuthorized=false;
 IOService* getAudioController(){if(audio)audio->retain();return audio;}
 void setProperty(const char*,uint64_t,unsigned){}
 void setProperty(const char*,const char*){}
 void setProperty(const char*,bool){}
 void resolvePlatformProfile();
};
'''
    actual = method(service_source(), 'CirrusAudioFixup', 'resolvePlatformProfile')
    checks = r'''
int main(){
 profileChecks();IOACPIPlatformDevice amp;IOPCIDevice audio;CirrusAudioFixup service;
 service.mProvider=&amp;service.audio=&audio;service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized&&service.mPlatformSubsystemId==0x103c89c6);
 Properties properties;service.mDiscoveredBoard=resolve(properties);assert(service.mDiscoveredBoard);
 service.mDiscoveredProfile=*service.mPlatformProfile;
 service.mDiscoveredProfile.boardConfiguration=&service.mDiscoveredBoard->configuration;
 service.mDiscoveredProfile.allowAutomaticInitialization=true;service.resolvePlatformProfile();
 assert(service.mPlatformAuthorized);
 assert(service.mPlatformProfile->identitySource==cirrus::platform::BoardIdentitySource::ACPI);
 // A PCI SSID alone must not substitute for the declared ACPI _SUB contract.
 amp.sub.value="INVALID";audio.ssid=0x103c89c6;service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 amp.sub.value="103C8CA4";service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 // Selecting HP first must not leak its identity, topology or channels into
 // either approved Lenovo board, even with a conflicting _SUB.
 for(auto ssid:{0x17aa382bU,0x17aa3847U}){
  amp.hid.value="CLSA0100";amp.sub.value="103C89C6";audio.ssid=ssid;service.resolvePlatformProfile();
  assert(service.mPlatformAuthorized&&service.mPlatformSubsystemId==ssid);
  const auto& legacy=*service.mPlatformProfile;
  assert(!strcmp(legacy.name,"lenovo-clsa0100-cs35l41"));
  assert(legacy.identitySource==cirrus::platform::BoardIdentitySource::AudioPCI);
  assert(!legacy.boardConfiguration&&legacy.endpointCount==2&&legacy.resetQuirk&&legacy.interruptWiring);
  assert(legacy.endpoints[0].address==0x40&&legacy.endpoints[0].channel==cirrus::core::AudioChannel::Left);
  assert(legacy.endpoints[1].address==0x41&&legacy.endpoints[1].channel==cirrus::core::AudioChannel::Right);
  audio.ssid=0x12345678;service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 }
 amp.hid.value="CLSA0101";audio.ssid=0x17aa382b;service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 amp.hid.value="CSC3551";amp.sub.value="103C89C6";service.resolvePlatformProfile();
 assert(service.mPlatformAuthorized&&service.mPlatformSubsystemId==0x103c89c6);
 assert(amp.hid.refs==1&&amp.sub.refs==1&&audio.refs==1);
 delete service.mDiscoveredBoard;
}
'''
    (tmp/'IOKit').mkdir()
    (tmp/'IOKit/IORegistryEntry.h').write_text('#pragma once\n')
    (tmp/'IOKit/IOLib.h').write_text('#pragma once\n')
    (tmp/'test.cpp').write_text('#include <string>\n'+fixture+mocks+actual+checks)
    exe = tmp / "test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"), str(ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/FirmwareData.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS OEM-independent ACPI configuration, topology validation, resource gates, Lenovo isolation and captured asset SHA-256")
