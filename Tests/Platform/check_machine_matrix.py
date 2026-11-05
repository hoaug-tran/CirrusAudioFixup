"""Replay supported board/resource contracts; no physical audio or bus simulation.

HP: issue #3 (103C89C6, R/L); HP ENVY x360 15-ey0xxx: issue #14 (103C8A31, L/R).
G614JI: issue #5 (10431C9F, L/R); GU605MV: issue #8 and gu605mv-asp-readback.json.
Lenovo ThinkBook 16p G6 IAX: issue #16 (17AA391E, L/R on I2C 0x31/0x30, broadcast 0x3C).
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
CPP = r"""
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
 ~Properties() override = default;
 bool contains(const char* key)const override{return values.count(key);}
 bool array(const char* key,uint32_t* out,size_t count)const override{
  auto i=values.find(key);if(i==values.end()||i->second.size()!=count)return false;
  std::copy(i->second.begin(),i->second.end(),out);return true;
 }
};
struct GPIO:platform::BoardDiscoveryContext::GPIOAccess {
 uint32_t speaker=0;bool readable=true;unsigned resets=0;
 ~GPIO() override = default;
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
  {"ASUS Zenbook UM3402Y",0x1043,0x1683,1,0,core::BusTransportType::I2C},
  {"Alienware x17 R2 speaker 0 / issue #12",0x1028,0x0b28,0,0,core::BusTransportType::SPI},
  {"Alienware x17 R2 speaker 1 / issue #12",0x1028,0x0b28,1,0,core::BusTransportType::SPI}};
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

  // 2. With GPIO access, discovery succeeds via production available check and BoardProperties
  std::unique_ptr<platform::DiscoveredBoard> found(
   l41::resolveBoardConfiguration({identity, endpoints, 2, p, &gpio}, available, reason));
  assert(found && found->configuration.permits(identity) && found->configuration.count == 2);
  assert(gpio.lastResource == 1); // speakerResource index 1 verified

  // Verify production firmware resources for both speaker ID 0 and 1
  const auto* res0 = l41::selectFirmwareResource(cs35l41Firmware, cs35l41FirmwareCount, 0x103c, 0x8a31, 0);
  assert(res0 && res0->wmfw && res0->wmfwSize);
  checkFirmware(*res0);
  const auto* res1 = l41::selectFirmwareResource(cs35l41Firmware, cs35l41FirmwareCount, 0x103c, 0x8a31, 1);
  assert(res1 && res1->wmfw && res1->wmfwSize);
  checkFirmware(*res1);

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
 // Lenovo ThinkBook 16p G6 IAX / issue #16 (Realtek ALC287 / 2x CS35L56 on I2C 0x31/0x30, broadcast 0x3C)
 {
  const auto* facts = l56::boardProperties(0x17aa, 0x391e);
  assert(facts && facts->count == 2);
  assert(facts->transport == core::BusTransportType::I2C);
  assert(facts->addresses[0] == 0x31 && facts->addresses[1] == 0x30);
  assert(facts->channels[0] == 0 && facts->channels[1] == 1);
  assert(facts->speakerId == 0 && facts->resetResource == -1);

  const auto* profile = platform::findPlatformProfile("CSC3556");
  assert(profile && profile->amplifierModel == core::AmplifierModel::CS35L56);

  core::BusEndpoint endpoints[] = {
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x31, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x30, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x3c, false, 0, 1000000, 0}
  };

  struct Lenovo_GPIO final : platform::BoardDiscoveryContext::GPIOAccess {
   unsigned resets = 0;
   int32_t lastReset = -1;
   ~Lenovo_GPIO() override = default;
   bool read(const char*, size_t, uint32_t& value) override { value = 0; return true; }
   bool reset(const char*, int32_t resource) override {
    ++resets;
    lastReset = resource;
    return true;
   }
  } gpio;

  const platform::BoardIdentity identity{0x17aa, 0x391e, "CSC3556"};
  const char* reason = nullptr;
  Properties p;

  // 1. Discovery succeeds with 3 endpoints in _CRS (0x31, 0x30, and ignored broadcast 0x3c)
  std::unique_ptr<platform::DiscoveredBoard> found(
   l56::resolveBoardConfiguration({identity, endpoints, 3, p, &gpio}, reason));
  assert(found && found->configuration.permits(identity) && found->configuration.count == 2);

  // 2. Hardware preparation succeeds without external GPIO reset (CS35L56 uses software reset via register bus)
  assert(found->prepareHardware(gpio));
  assert(gpio.resets == 0 && gpio.lastReset == -1);

  // 3. Verify production firmware resource and validate WMFW + channel coefficient BINs
  const auto* resource = l56::selectFirmwareResource(l56::cs35l56Firmware, l56::cs35l56FirmwareCount, 0x17aa, 0x391e, 0);
  assert(resource && resource->wmfw && resource->wmfwSize);
  checkFirmware(*resource);

  // 4. Validate amplifier endpoints and channel bindings: 0x31 is Left (channel 0), 0x30 is Right (channel 1)
  for (size_t i = 0; i < 2; ++i) {
   const auto& binding = found->configuration.amplifiers[i];
   assert(binding.endpoint.transport == core::BusTransportType::I2C);
   assert(binding.endpoint.i2cAddress == facts->addresses[i]);
   assert(binding.channelIndex == facts->channels[i]);
   assert(binding.speakerId == 0);
   assert(!strcmp(binding.endpoint.controllerPath, "\\_SB.PC02.I2C0.SPKR"));
  }

  // 5. Test order-independence: endpoints with 0x30 preceding 0x31
  core::BusEndpoint swappedEndpoints[] = {
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x30, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x3c, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.PC02.I2C0.SPKR", 0x31, false, 0, 1000000, 0}
  };
  std::unique_ptr<platform::DiscoveredBoard> foundSwapped(
   l56::resolveBoardConfiguration({identity, swappedEndpoints, 3, p, &gpio}, reason));
  assert(foundSwapped && foundSwapped->configuration.permits(identity) && foundSwapped->configuration.count == 2);
  assert(foundSwapped->configuration.amplifiers[0].endpoint.i2cAddress == 0x31 &&
         foundSwapped->configuration.amplifiers[0].channelIndex == 0);
  assert(foundSwapped->configuration.amplifiers[1].endpoint.i2cAddress == 0x30 &&
         foundSwapped->configuration.amplifiers[1].channelIndex == 1);

  // 6. Permitting and mismatch rejection
  auto wrong = identity;
  wrong.subsystemDevice ^= 1;
  assert(!found->configuration.permits(wrong));

  // 7. Unsupported/unverified board (Yoga 9 17AA:390B) fails closed without tuning
  const platform::BoardIdentity yoga9Identity{0x17aa, 0x390b, "CSC3556"};
  assert(!l56::resolveBoardConfiguration({yoga9Identity, endpoints, 3, p, &gpio}, reason));
  assert(!strcmp(reason, "CS35L56_SPEAKER_TUNING_UNAVAILABLE"));

  printf("PASS Lenovo ThinkBook 16p G6 IAX issue #16: I2C dual-amp (0x31/0x30, 0x3C ignored), DSP software reset, firmware 17AA:391E\n");
 }
 // ASUS ROG Zephyrus G14 GA403UV / issue #18 (Realtek ALC285 / 2x CS35L56 on I2C 0x31/0x30, broadcast 0x3A)
 {
  const auto* facts = l56::boardProperties(0x1043, 0x1b13);
  assert(facts && facts->count == 2);
  assert(facts->transport == core::BusTransportType::I2C);
  assert(facts->addresses[0] == 0x31 && facts->addresses[1] == 0x30);
  assert(facts->channels[0] == 0 && facts->channels[1] == 1);
  assert(facts->speakerId == 0 && facts->resetResource == -1);

  const auto* profile = platform::findPlatformProfile("CSC3556");
  assert(profile && profile->amplifierModel == core::AmplifierModel::CS35L56);

  core::BusEndpoint endpoints[] = {
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x31, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x30, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x3a, false, 0, 1000000, 0}
  };

  struct ASUS_GPIO final : platform::BoardDiscoveryContext::GPIOAccess {
   unsigned resets = 0;
   int32_t lastReset = -1;
   ~ASUS_GPIO() override = default;
   bool read(const char*, size_t, uint32_t& value) override { value = 0; return true; }
   bool reset(const char*, int32_t resource) override {
    ++resets;
    lastReset = resource;
    return true;
   }
  } gpio;

  const platform::BoardIdentity identity{0x1043, 0x1b13, "CSC3556"};
  const char* reason = nullptr;
  Properties p;

  // 1. Discovery succeeds with 3 endpoints in _CRS (0x31, 0x30, and ignored broadcast 0x3a)
  std::unique_ptr<platform::DiscoveredBoard> found(
   l56::resolveBoardConfiguration({identity, endpoints, 3, p, &gpio}, reason));
  assert(found && found->configuration.permits(identity) && found->configuration.count == 2);

  // 2. Hardware preparation succeeds without external GPIO reset (CS35L56 uses software reset via register bus)
  assert(found->prepareHardware(gpio));
  assert(gpio.resets == 0 && gpio.lastReset == -1);

  // 3. Verify production firmware resource and validate WMFW + channel coefficient BINs
  const auto* resource = l56::selectFirmwareResource(l56::cs35l56Firmware, l56::cs35l56FirmwareCount, 0x1043, 0x1b13, 0);
  assert(resource && resource->wmfw && resource->wmfwSize);
  checkFirmware(*resource);

  // 4. Validate amplifier endpoints and channel bindings: 0x31 is Left (channel 0), 0x30 is Right (channel 1)
  for (size_t i = 0; i < 2; ++i) {
   const auto& binding = found->configuration.amplifiers[i];
   assert(binding.endpoint.transport == core::BusTransportType::I2C);
   assert(binding.endpoint.i2cAddress == facts->addresses[i]);
   assert(binding.channelIndex == facts->channels[i]);
   assert(binding.speakerId == 0);
   assert(!strcmp(binding.endpoint.controllerPath, "\\_SB.I2CB.SPKR"));
  }

  // 5. Test order-independence: endpoints with 0x30 preceding 0x31
  core::BusEndpoint swappedEndpoints[] = {
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x30, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x3a, false, 0, 1000000, 0},
   {core::BusTransportType::I2C, "\\_SB.I2CB.SPKR", 0x31, false, 0, 1000000, 0}
  };
  std::unique_ptr<platform::DiscoveredBoard> foundSwapped(
   l56::resolveBoardConfiguration({identity, swappedEndpoints, 3, p, &gpio}, reason));
  assert(foundSwapped && foundSwapped->configuration.permits(identity) && foundSwapped->configuration.count == 2);
  assert(foundSwapped->configuration.amplifiers[0].endpoint.i2cAddress == 0x31 &&
         foundSwapped->configuration.amplifiers[0].channelIndex == 0);
  assert(foundSwapped->configuration.amplifiers[1].endpoint.i2cAddress == 0x30 &&
         foundSwapped->configuration.amplifiers[1].channelIndex == 1);

  // 6. Permitting and mismatch rejection
  auto wrong = identity;
  wrong.subsystemDevice ^= 1;
  assert(!found->configuration.permits(wrong));

  printf("PASS ASUS ROG Zephyrus G14 GA403UV issue #18: I2C dual-amp (0x31/0x30, 0x3A ignored), DSP software reset, firmware 1043:1B13\n");
 }
}
"""
with tempfile.TemporaryDirectory(prefix="cirrus-machine-matrix-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(CPP)
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                *flags,
                "-I" + str(tmp),
                "-I" + str(ROOT / "CirrusAudioFixup"),
                str(tmp / "test.cpp"),
                *[
                    str(
                        ROOT
                        / f"CirrusAudioFixup/Devices/Amplifiers/{model}/Resources/FirmwareData.cpp"
                    )
                    for model in ("CS35L41", "CS35L56")
                ],
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
