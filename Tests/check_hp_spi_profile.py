"""Exact HP board identity, channel/asset bindings and captured firmware hashes."""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile
from production_source_fixture import method

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/Firmware.hpp").read_text()
for symbol, size, digest in (
    ("cs35l41_dsp1_spk_prot_103c89c6_wmfw", 34068, "c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4"),
    ("cs35l41_dsp1_spk_prot_103c89c6_spkid0_l0_bin", 4000, "7b6a432787c14a9ba1ec419672c9ff87d0101669ecda37dc14c8e902ed363c42"),
    ("cs35l41_dsp1_spk_prot_103c89c6_spkid0_r0_bin", 4000, "2f7e769606a77ef433e5c1ca35bb4e5679b2d01326aefd9fdc11cdfcb151d81d"),
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
#include "Platform/PlatformProfile.hpp"
void profileChecks(){
 using namespace cirrus;uint32_t identity=7;
 assert(platform::parseACPISubsystemId("103C89C6",identity)&&identity==0x103c89c6);
 assert(platform::parseACPISubsystemId("103c89c6",identity));
 for(auto bad:{"", "103C89C", "0x103C89C6", "103G89C6", "000089C6", "FFFFFFFF"})
  assert(!platform::parseACPISubsystemId(bad,identity)&&identity==0x103c89c6);
 auto* profile=platform::findPlatformProfile("CSC3551",identity);
 assert(profile&&profile->identitySource==platform::BoardIdentitySource::ACPI);
 assert(profile->boardConfiguration&&profile->boardConfiguration->useBoardFirmwareIdentity);
 assert(platform::permitsHardwareInitialization(*profile,identity));
 assert(!platform::permitsHardwareInitialization(*profile,0x103c8ca4));
 assert(!platform::permitsHardwareInitialization(*profile,0x17aa3847));
 const auto& board=*profile->boardConfiguration;assert(board.count==2);
 assert(board.amplifiers[0].channelIndex==uint32_t(core::AudioChannel::Right));
 assert(board.amplifiers[1].channelIndex==uint32_t(core::AudioChannel::Left));
 for(unsigned i=0;i<2;++i){
  const auto& a=board.amplifiers[i];assert(a.endpoint.valid());
  assert(a.endpoint.transport==core::BusTransportType::SPI&&a.endpoint.spiChipSelect==i);
  assert(a.endpoint.spiClockHz==4000000&&a.endpoint.spiMode==0&&a.siliconId==0x35a40);
  assert(!strcmp(a.endpoint.controllerPath,"\x5c_SB.PC00.SPI0"));
  auto* power=devices::amplifiers::cs35l41::powerConfiguration(a.driverConfiguration,a.driverConfigurationSize,a.driverConfigurationKind);
  assert(power&&power->internal()&&power->inductorNanohenry==1000&&power->peakMilliamp==4500&&power->capacitorMicrofarad==24);
 }
 assert(strstr(board.amplifiers[0].tuningResource,"-r0.bin"));
 assert(strstr(board.amplifiers[1].tuningResource,"-l0.bin"));
 assert(!platform::findPlatformProfile("CSC3551")->allowAutomaticInitialization);
 assert(!platform::findPlatformProfile("CSC3551",0x103c8ca4)->allowAutomaticInitialization);
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
struct IOService:IORegistryEntry {OSObject* getProperty(const char*){return nullptr;}const char* getName(){return "unrecognized";}};
struct IOACPIPlatformDevice:IOService {OSString hid{"CSC3551"},sub{"103C89C6"};
 int evaluateObject(const char* name,OSObject** out){*out=!strcmp(name,"_HID")?&hid:!strcmp(name,"_SUB")?&sub:nullptr;if(!*out)return -1;(*out)->retain();return 0;}};
struct IOPCIDevice:IOService {uint32_t ssid=0x103c8ca4;uint16_t configRead16(unsigned offset){return offset==0x2c?ssid>>16:ssid&0xffff;}};
#include "Support/KernelDeviceCast.hpp"
#define CIRRUS_LOG(...) ((void)0)
class CirrusAudioFixup {
public:
 IOService* mProvider=nullptr;IOPCIDevice* audio=nullptr;
 const cirrus::platform::PlatformProfile* mPlatformProfile=nullptr;
 uint32_t mPlatformSubsystemId=0;bool mPlatformAuthorized=false;
 IOService* getAudioController(){if(audio)audio->retain();return audio;}
 void setProperty(const char*,uint64_t,unsigned){}
 void setProperty(const char*,const char*){}
 void setProperty(const char*,bool){}
 void resolvePlatformProfile();
};
'''
    actual = method((ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(), 'CirrusAudioFixup', 'resolvePlatformProfile')
    checks = r'''
int main(){
 profileChecks();IOACPIPlatformDevice amp;IOPCIDevice audio;CirrusAudioFixup service;
 service.mProvider=&amp;service.audio=&audio;service.resolvePlatformProfile();
 assert(service.mPlatformAuthorized&&service.mPlatformSubsystemId==0x103c89c6);
 assert(service.mPlatformProfile->identitySource==cirrus::platform::BoardIdentitySource::ACPI);
 // A PCI SSID alone must not substitute for the declared ACPI _SUB contract.
 amp.sub.value="INVALID";audio.ssid=0x103c89c6;service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 amp.sub.value="103C8CA4";service.resolvePlatformProfile();assert(!service.mPlatformAuthorized);
 // Selecting HP first must not leak its identity, topology or channels into
 // either previously supported Lenovo board, even with a conflicting _SUB.
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
}
'''
    (tmp/'IOKit').mkdir()
    (tmp/'IOKit/IORegistryEntry.h').write_text('#pragma once\n')
    (tmp/'test.cpp').write_text('#include <string>\n'+fixture+mocks+actual+checks)
    exe = tmp / "test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS HP and legacy Lenovo identity isolation, endpoints, power configuration and captured asset SHA-256")
