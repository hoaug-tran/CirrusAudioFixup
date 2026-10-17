"""Execute the service's real discovery callback over evaluated ACPI packages."""
from pathlib import Path
import subprocess
import tempfile
from acpi_kernel_fixture import install
from production_source_fixture import method

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cirrus-platform-discovery-") as directory:
    tmp = Path(directory)
    install(tmp)
    owner = method((ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text(), "CirrusAudioFixup", "discoverPlatformConfiguration")
    source = r'''
#define KERNEL 1
#include "Fixture.hpp"
#include "Platform/PlatformProfile.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include "Transport/ACPIPlatformProperties.hpp"
#include "Devices/Amplifiers/CS35L41/PlatformConfiguration.hpp"
using namespace cirrus;
bool resourceAvailable=true;unsigned callbacks=0;
bool resources(uint16_t,uint16_t,uint32_t){return resourceAvailable;}
platform::DiscoveredBoard* discover(const platform::BoardDiscoveryContext& context,const char*& reason){
 ++callbacks;return devices::amplifiers::cs35l41::resolvePlatformConfiguration(context,resources,reason);
}
struct Native {
 core::BusEndpoint endpoints[2]{{core::BusTransportType::SPI,"\\_SB.NEW.SPI8",0,false,0,4000000,0},
                               {core::BusTransportType::SPI,"\\_SB.NEW.SPI8",0,false,1,4000000,0}};
 size_t count()const{return 2;}core::BusEndpoint endpoint(size_t i)const{return endpoints[i];}
};
class CirrusAudioFixup {
public:
 static constexpr size_t kMaxAmplifiers=16;
 Native mNativeSPI;
 const platform::PlatformProfile* mPlatformProfile=platform::findPlatformProfile("CSC3551");
 platform::PlatformProfile mDiscoveredProfile{};
 platform::DiscoveredBoard* mDiscoveredBoard=nullptr;
 uint32_t mPlatformSubsystemId=0x103c89c6;bool mPlatformAuthorized=false;
 std::map<std::string,std::string> strings;
 void setProperty(const char* name,const char* value){strings[name]=value;}
 void discoverPlatformConfiguration(IOACPIPlatformDevice*,const platform::AmplifierDriverRegistration&);
 ~CirrusAudioFixup(){delete mDiscoveredBoard;}
};
OSArray* pair(const char* name,std::initializer_list<uint32_t> values){
 std::vector<OSObject*> items;for(auto value:values)items.push_back(make<OSNumber>(value));
 return make<OSArray>(std::vector<OSObject*>{make<OSString>(name),make<OSArray>(items)});
}
OSArray* dsd(OSArray* entries){
 return make<OSArray>(std::vector<OSObject*>{make<OSData>(std::vector<uint8_t>{0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01}),entries});
}
''' + owner + r'''
int main(){
 IOACPIPlatformDevice amp;
 auto* entries=make<OSArray>(std::vector<OSObject*>{pair("cirrus,dev-index",{0,1}),pair("cirrus,speaker-position",{1,0}),
  pair("cirrus,boost-type",{0,0}),pair("cirrus,gpio1-func",{0,0}),pair("cirrus,gpio2-func",{2,2}),
  pair("cirrus,boost-ind-nanohenry",{1000,1000}),pair("cirrus,boost-peak-milliamp",{4500,4500}),
  pair("cirrus,boost-cap-microfarad",{24,24})});
 auto* package=dsd(entries);amp.values["_DSD"]=package;
 platform::AmplifierDriverRegistration registration{};registration.discoverBoard=discover;
 {
  CirrusAudioFixup service;service.discoverPlatformConfiguration(&amp,registration);
  assert(callbacks==1&&service.mDiscoveredBoard&&service.mPlatformAuthorized);
  assert(service.mPlatformProfile==&service.mDiscoveredProfile);
  assert(service.strings["Cirrus_Platform_Profile"]=="acpi-device-properties");
  const auto& board=*service.mPlatformProfile->boardConfiguration;
  assert(board.count==2&&board.amplifiers[0].channelIndex==1&&board.amplifiers[1].channelIndex==0);
  assert(board.amplifiers[0].endpoint.controllerPath!=service.mNativeSPI.endpoints[0].controllerPath);
  assert(!strcmp(board.amplifiers[0].endpoint.controllerPath,service.mNativeSPI.endpoints[0].controllerPath));
  assert(package->refs==1);
 }
 {CirrusAudioFixup service;resourceAvailable=false;service.discoverPlatformConfiguration(&amp,registration);
  assert(!service.mPlatformAuthorized&&!service.mDiscoveredBoard);
  assert(service.strings["Cirrus_Platform_Discovery"]=="CS35L41_GENERIC_TUNING_UNAVAILABLE");resourceAvailable=true;}
 {CirrusAudioFixup service;service.mPlatformSubsystemId=0;service.discoverPlatformConfiguration(&amp,registration);
  assert(!service.mPlatformAuthorized&&!service.mDiscoveredBoard);}
 // Truncated, non-integer and oversized arrays cannot become a writable board.
 {transport::ACPIDeviceProperties properties;assert(properties.load(&amp)==0);transport::ACPIPlatformProperties source(properties);
  uint32_t value[2]{};assert(source.array("cirrus,dev-index",value,2)&&value[1]==1);
  assert(!source.array("cirrus,dev-index",value,1));
  auto* values=OSDynamicCast(OSArray,entries->getObject(0));values=OSDynamicCast(OSArray,values->getObject(1));
  values->v[1]=make<OSNumber>(uint64_t(1)<<32);assert(!source.array("cirrus,dev-index",value,2));
  values->v[1]=make<OSString>("1");assert(!source.array("cirrus,dev-index",value,2));values->v[1]=make<OSNumber>(1);}
 entries->v.push_back(pair("cirrus,dev-index",{0,1}));
 {CirrusAudioFixup service;const auto before=callbacks;service.discoverPlatformConfiguration(&amp,registration);
  assert(callbacks==before&&!service.mPlatformAuthorized&&!service.mDiscoveredBoard);}
 assert(package->refs==1);
}
'''
    (tmp / "test.cpp").write_text(source)
    for flags in ([], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS real service discovery callback, evaluated ACPI arrays, owned endpoints, resource gate and malformed input cleanup")
