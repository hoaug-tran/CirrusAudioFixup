"""Replay supported board/resource contracts; no physical audio or bus simulation.

HP: issue #3 (103C89C6, R/L); HP ENVY x360 15-ey0xxx: issue #14 (103C8A31, L/R).
G614JI: issue #5 (10431C9F, L/R); GU605MV: issue #8 and gu605mv-asp-readback.json.
Zenbook: captured fixture; G614JV issues #2/#4 lack verified SSID/tuning: test rejection.
Legacy Lenovo endpoints remain covered by the complete driver lifecycle suite.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
from driver_kernel_fixture import install
from host_build import sanitizer_flags

ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <cassert>
#include <cstdio>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "Devices/Amplifiers/CS35L41/Board/CS35L41BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareData.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
#include "Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
#include "Platform/Board/PlatformProfile.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"
using namespace cirrus;
namespace l41=devices::amplifiers::cs35l41;
namespace l56=devices::amplifiers::cs35l56;
struct Properties:platform::PlatformProperties {
 std::map<std::string,std::vector<uint32_t>> values;
 bool contains(const char* key)const override{return values.count(key);}
 bool array(const char* key,uint32_t* out,size_t count)const override{
  auto i=values.find(key);if(i==values.end()||i->second.size()!=count)return false;
  std::copy(i->second.begin(),i->second.end(),out);return true;
 }
};
struct GPIO:platform::BoardDiscoveryContext::GPIOAccess {
 uint32_t speaker=0;bool readable=true;unsigned resets=0;
 bool read(const char*,size_t,uint32_t& value)override{value=speaker;return readable;}
 bool reset(const char*,int32_t)override{++resets;return true;}
};
bool available(uint16_t vendor,uint16_t device,uint32_t speaker){
 return l41::selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,vendor,device,speaker)!=nullptr;
}
template<class Resource> void checkFirmware(const Resource& resource){
 auto image=std::make_unique<FirmwareImage>();auto mapped=std::make_unique<MappedImage>();
 assert(!resource.isDummy&&WMFWParser::parseWMFW(resource.wmfw,resource.wmfwSize,image.get()));
 assert(WMFWMemoryMapper::mapFirmwareImage(*image,*mapped));
 // Coefficient addresses require the live DSP algorithm table; parse both assets here.
 for(const bool right:{false,true}){
  size_t bytes=0;const auto* data=l41::selectChannelCoefficients(resource,right,bytes);
  assert(data&&bytes&&WMFWParser::parseBIN(data,bytes,image.get()));
  assert(image->coefficientCount && image->totalCoeffPayloadBytes);
 }
}
int main(){
 for(const uint16_t ssid:{uint16_t(0x382b),uint16_t(0x3847)}){
  const auto* profile=platform::findPlatformProfile("CLSA0100");
  assert(profile&&platform::permitsHardwareInitialization(*profile,0x17aa0000U|ssid));
  assert(profile->endpointCount==2&&profile->endpoints[0].address==0x40&&profile->endpoints[1].address==0x41);
  // Existing driver translates 382B to the verified 3847 resource; tested in check_cs35l41_driver.
  const auto* resource=l41::selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,0x17aa,0x3847,1);
  assert(resource);checkFirmware(*resource);
  printf("PASS Lenovo Legion board/resources: 17AA:%04X\n",unsigned(ssid));
 }
 struct Board {const char* name;uint16_t vendor,device;uint32_t speaker,leftFirst;core::BusTransportType bus;};
 const Board boards[]{
  {"HP ZBook Fury 16 G9 / issue #3",0x103c,0x89c6,UINT32_MAX,1,core::BusTransportType::SPI},
  {"ASUS G614JI speaker 0 / issue #5",0x1043,0x1c9f,0,0,core::BusTransportType::SPI},
  {"ASUS G614JI speaker 1 / issue #5",0x1043,0x1c9f,1,0,core::BusTransportType::SPI},
  {"ASUS Zenbook UM3402Y",0x1043,0x1683,1,0,core::BusTransportType::I2C}};
 for(const auto& board:boards)for(bool reverse:{false,true}){
  Properties p;p.values={{"cirrus,dev-index",{0,1}},{"cirrus,speaker-position",{board.leftFirst,1-board.leftFirst}},
   {"cirrus,boost-type",{0,0}},{"cirrus,gpio1-func",{0,0}},{"cirrus,gpio2-func",{2,2}},
   {"cirrus,boost-ind-nanohenry",{1000,1000}},{"cirrus,boost-peak-milliamp",{4500,4500}},
   {"cirrus,boost-cap-microfarad",{24,24}}};
  if(board.bus==core::BusTransportType::I2C)p.values["cirrus,gpio1-func"]={1,1};
  if(board.speaker!=UINT32_MAX)p.values["cirrus,speaker-id"]={board.speaker,board.speaker};
  core::BusEndpoint endpoints[]={{board.bus,"controller",0x40,false,0,4000000,0},
                                {board.bus,"controller",0x41,false,1,4000000,0}};
  // I2C indexed metadata follows the provider's order; SPI follows chip select.
  if(reverse&&board.bus==core::BusTransportType::SPI)std::swap(endpoints[0],endpoints[1]);
  const platform::BoardIdentity identity{board.vendor,board.device,"CSC3551"};const char* reason=nullptr;
  std::unique_ptr<platform::DiscoveredBoard> found(l41::resolveBoardConfiguration({identity,endpoints,2,p},available,reason));
  if(!found)fprintf(stderr,"%s: %s\n",board.name,reason);
  assert(found&&found->configuration.permits(identity)&&found->configuration.count==2);
  for(size_t i=0;i<2;++i){
   const auto& binding=found->configuration.amplifiers[i];
   const auto physical=board.bus==core::BusTransportType::SPI?endpoints[i].spiChipSelect:uint32_t(i);
   assert(binding.channelIndex==(physical?1-board.leftFirst:board.leftFirst)&&binding.speakerId==board.speaker);
   l41::DeviceConfiguration config;
   assert(l41::decodeDeviceConfiguration(binding.driverConfiguration,binding.driverConfigurationSize,binding.driverConfigurationKind,config));
   assert(config.power.switched()==(board.bus==core::BusTransportType::I2C));
   const auto* resource=l41::selectFirmwareResource(cs35l41Firmware,cs35l41FirmwareCount,board.vendor,board.device,binding.speakerId);
   assert(resource);checkFirmware(*resource);
  }
  auto wrong=identity;wrong.subsystemDevice^=1;assert(!found->configuration.permits(wrong));
  printf("PASS board/resource replay: %s order=%u\n",board.name,unsigned(reverse));
 }
 // HP ENVY x360 15-ey0xxx / issue #14 (Realtek ALC245 / CS35L41 dual-amp on I2C 0x40/0x41)
 {
  const auto* facts = l41::boardProperties(0x103c, 0x8a31);
  assert(facts && facts->count == 2 && facts->internalBoost);
  assert(facts->channels[0] == 0 && facts->channels[1] == 1);
  assert(facts->resetResource == 0 && facts->speakerResource == 1 && facts->csResource == -1);
  assert(facts->inductor == 1000 && facts->peak == 4100 && facts->capacitor == 24);

  const auto* profile = platform::findPlatformProfile("CSC3551");
  assert(profile && profile->amplifierModel == core::AmplifierModel::CS35L41);

  core::BusEndpoint endpoints[] = {
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x40, false, 0, 400000, 0},
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x41, false, 0, 400000, 0}
  };
  struct HP_GPIO final : platform::BoardDiscoveryContext::GPIOAccess {
   unsigned resets = 0;
   int32_t lastReset = -1;
   uint32_t lastResource = UINT32_MAX;
   bool read(const char*, size_t, uint32_t& value) override { value = 0; return true; }
   bool readResource(uint32_t resource, uint32_t pin, bool low, uint32_t& value) override {
    (void)pin; (void)low;
    lastResource = resource;
    value = 0;
    return true;
   }
   bool reset(const char*, int32_t resource) override {
    ++resets;
    lastReset = resource;
    return true;
   }
  } gpio;

  const platform::BoardIdentity identity{0x103c, 0x8a31, "CSC3551"};
  const char* reason = nullptr;
  Properties p;

  // 1. Without GPIO access, speakerResource >= 0 fails closed
  assert(!l41::resolveBoardConfiguration({identity, endpoints, 2, p}, available, reason));
  assert(!strcmp(reason, "CS35L41_SPEAKER_ID_REQUIRED"));

  // 2. With GPIO access, discovery succeeds via BoardProperties fallback
  auto hpAvailable = [](uint16_t v, uint16_t d, uint32_t) { return v == 0x103c && d == 0x8a31; };
  std::unique_ptr<platform::DiscoveredBoard> found(
   l41::resolveBoardConfiguration({identity, endpoints, 2, p, &gpio}, hpAvailable, reason));
  assert(found && found->configuration.permits(identity) && found->configuration.count == 2);
  assert(gpio.lastResource == 1); // speakerResource index 1 verified

  // 3. Hardware preparation triggers reset on resetResource index 0
  assert(found->prepareHardware(gpio));
  assert(gpio.resets == 1 && gpio.lastReset == 0);

  // 4. Validate amplifier endpoints, channels, and electrical power config
  for (size_t i = 0; i < 2; ++i) {
   const auto& binding = found->configuration.amplifiers[i];
   assert(binding.endpoint.transport == core::BusTransportType::I2C);
   assert(binding.endpoint.i2cAddress == 0x40 + i);
   assert(binding.channelIndex == i);
   l41::DeviceConfiguration config;
   assert(l41::decodeDeviceConfiguration(binding.driverConfiguration, binding.driverConfigurationSize,
                                         binding.driverConfigurationKind, config));
   assert(config.power.internal() && !config.power.switched());
   assert(config.power.inductorNanohenry == 1000);
   assert(config.power.peakMilliamp == 4100);
   assert(config.power.capacitorMicrofarad == 24);
   assert(config.gpio1Function == 0 && config.gpio2Function == 2);
  }

  // 5. Permitting and mismatch rejection
  auto wrong = identity;
  wrong.subsystemDevice ^= 1;
  assert(!found->configuration.permits(wrong));
  printf("PASS HP ENVY x360 15-ey0xxx issue #14: I2C dual-amp (0x40/0x41), internal boost (1000nH/4100mA/24uF), GPIO reset(0)/speaker(1)\n");
 }
 // Reported G614JV identity/tuning gaps must remain unauthorized.
 Properties empty;core::BusEndpoint unknown[]={{core::BusTransportType::SPI,"controller",0,false,0,4000000,0},
                                             {core::BusTransportType::SPI,"controller",0,false,1,4000000,0}};
 const char* reason=nullptr;
 assert(!l41::resolveBoardConfiguration({{0x1043,0,"CSC3551"},unknown,2,empty},available,reason));
 assert(!available(0x1043,0,1)&&!available(0x1043,0x1c9f,UINT32_MAX));
 printf("PASS G614JV issues #2/#4: incomplete identity/tuning rejected\n");
 for(bool reverse:{false,true}){
  Properties p;p.values={{"cirrus,dev-index",{0,1}},{"cirrus,boost-type",{0,0}},{"spk-id-gpios",{0,0}}};GPIO gpio;
  if(reverse)std::swap(unknown[0],unknown[1]);
  const platform::BoardIdentity identity{0x1043,0x1c63,"CSC3556"};
  std::unique_ptr<platform::DiscoveredBoard> found(l56::resolveBoardConfiguration({identity,unknown,2,p,&gpio},reason));
  assert(found&&found->configuration.permits(identity)&&!gpio.resets);
  for(size_t i=0;i<2;++i)assert(found->configuration.amplifiers[i].channelIndex==unknown[i].spiChipSelect);
  const auto* resource=l56::selectFirmwareResource(l56::cs35l56Firmware,l56::cs35l56FirmwareCount,0x1043,0x1c63,0);
  assert(resource);checkFirmware(*resource);
  gpio.speaker=1;assert(!l56::resolveBoardConfiguration({identity,unknown,2,p,&gpio},reason));
  gpio.readable=false;assert(!l56::resolveBoardConfiguration({identity,unknown,2,p,&gpio},reason));
 }
 printf("PASS GU605MV issue #8: SPI order, distinct coefficients, speaker gates; hardware untested\n");
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-machine-matrix-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(CPP)
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I"+str(tmp), "-I"+str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"),
                        *[str(ROOT / f"CirrusAudioFixup/Devices/Amplifiers/{model}/Resources/FirmwareData.cpp")
                          for model in ("CS35L41", "CS35L56")], "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
