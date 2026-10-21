"""Internal/external CS35L41 power sequences with real playback state machines."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-power-") as directory:
    tmp = Path(directory)
    (tmp / "test.cpp").write_text(r'''
#include <cassert>
#include <map>
#include <vector>
#include <algorithm>
#include "Devices/Amplifiers/CS35L41/Playback.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41Device.hpp"
using namespace cirrus::devices::amplifiers::cs35l41;
struct Port:cirrus::core::RegisterIO {
 std::map<uint32_t,uint32_t> r;std::vector<uint32_t> writes;unsigned calls=0,failAt=0;
 bool step(){return ++calls!=failAt;}
 bool read(uint32_t reg,uint32_t* out)override{if(!step())return false;*out=r[reg];return true;}
 bool write(uint32_t reg,uint32_t value)override{
  // Linux CSPL commands used by this lifecycle
  if(reg==0x13020)assert(value==1||value==2||value==7);
  if(!step())return false;
  writes.push_back(reg);
  if(reg==0x10010)r[reg]&=~value;else r[reg]=value;
  if(reg==0x2014)r[0x10010]|=(value&1)?0x01000000:0x00800000;
  if(reg==0x13020)r[0x13004]=(value==1)?1:0;
  return true;
 }
 bool updateBits(uint32_t reg,uint32_t mask,uint32_t value)override{uint32_t old=0;return read(reg,&old)&&write(reg,(old&~mask)|(value&mask));}
 bool bulkRead(uint32_t,uint8_t*,size_t)override{return false;}
 bool bulkWrite(uint32_t,const uint8_t*,size_t)override{return false;}
 bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t)override{return false;}
};
int main(){
 {Port io;io.r[0x2018]=0x3021;CS35L41Device device("helper",0x40);
  assert(device.stopPlayback(io));
  assert(!(io.r[0x2018]&1)&&io.r[0x6000]==0xA678&&io.r[0x6c04]==0);
  // Monitor/boost still active: never claim shutdown.
  assert(!device.powerDown(io));
  io.r[0x2018]=0;io.r[0x2014]=0;assert(device.powerDown(io));
  io.r[0]=UINT32_MAX;assert(!device.isAlive(io));
  io.r[0]=registers::kValDeviceId;assert(device.isAlive(io));
  auto before=io.calls;assert(!device.startPlayback(io)&&io.calls==before);
  for(unsigned failure=1;failure<=4;++failure){Port broken;broken.failAt=failure;assert(!device.stopPlayback(broken));}
 }
 const PowerConfiguration internal{BoostTopology::Internal,1000,4500,24};
 assert(internal.valid()&&internal.activePower(true)==0x3021);
 assert(kLegacyPowerConfiguration.activePower(true)==0x3001);
 Port coefficients;assert(internal.configure(coefficients));
 // These are the actual Linux regmap values for both captured HP amps.
 assert(coefficients.r[0x3810]==0x4932&&coefficients.r[0x3814]==0x7500&&coefficients.r[0x3808]==0x4a);
 for(unsigned failure=1;failure<=10;++failure){Port broken;broken.failAt=failure;assert(!internal.configure(broken));}
 Port active;active.r[0x2014]=1;assert(!internal.configure(active)&&active.writes.empty());
 auto invalid=internal;invalid.peakMilliamp=4501;Port untouched;assert(!invalid.configure(untouched)&&untouched.writes.empty());
 invalid=internal;invalid.inductorNanohenry=900;assert(!invalid.configure(untouched)&&untouched.writes.empty());
 assert(!powerConfiguration(&internal,sizeof(internal),"other-schema"));
 assert(powerConfiguration(&internal,sizeof(internal),kPowerConfigurationKind)==&internal);
 assert(powerConfiguration(nullptr,0,nullptr)==&kLegacyPowerConfiguration);
 Port legacyConfiguration;assert(kLegacyPowerConfiguration.configure(legacyConfiguration));
 // Old bindings gain no boost programming I/O.
 assert(legacyConfiguration.calls==0);
 // Run legacy default, explicit external and internal lifecycles interleaved:
 // no global power selection may leak between independent amplifier instances.
 for(bool dsp:{false,true}){
  Port legacy,explicitLegacy,hp;
  // Seed the previous DSP path: bypass must not rely on power-on defaults.
  for(auto* io:{&legacy,&explicitLegacy,&hp})io->r[0x4C54]=0x29;
  for(auto* io:{&legacy,&explicitLegacy,&hp})io->r[0x10098]=2;
  PlaybackTransition a{},b{},c{};
  PlaybackProgress ar=PlaybackProgress::Pending,br=ar,cr=ar;
  for(uint64_t now=0;now<20;++now){
   if(ar==PlaybackProgress::Pending)ar=Playback::advance(legacy,a,now,dsp,0x2b01);
   if(cr==PlaybackProgress::Pending)cr=Playback::advance(hp,c,now,dsp,0x2b01,nullptr,internal);
   if(br==PlaybackProgress::Pending)br=Playback::advance(explicitLegacy,b,now,dsp,0x2b01,nullptr,kLegacyPowerConfiguration);
  }
  assert(ar==PlaybackProgress::Prepared&&br==ar&&cr==ar);
  assert(hp.r[0x4C50]==(dsp?0x28U:0x20U));
  assert(legacy.r[0x4C50]==(dsp?0x29U:0x20U));
  if(!dsp){
   assert(hp.r[0x4C54]==0x21&&legacy.r[0x4C54]==0x21);
   assert(hp.r[0x4C28]==0&&hp.r[0x4C2C]==0);
   auto damaged=hp;auto transition=c;damaged.r[0x4C54]=0x29;
   assert(!Playback::commit(damaged,transition,17<<5,false,internal));
   assert(damaged.r[0x6000]==0xA678);
  }
  if(dsp){
   assert(hp.r[0x4C54]==0x29U&&legacy.r[0x4C54]==0x29U&&explicitLegacy.r[0x4C54]==0x29U);
   assert(hp.r[0x4C58]==0x3AU&&legacy.r[0x4C58]==0x3AU&&explicitLegacy.r[0x4C58]==0x3AU);
   assert(hp.r[0x4C5C]==0x3BU&&legacy.r[0x4C5C]==0x3BU&&explicitLegacy.r[0x4C5C]==0x3BU);
   auto wrongInternal=hp;auto wrongExternal=legacy;auto internalTransition=c;auto externalTransition=a;
   wrongInternal.r[0x4C54]=0x21;wrongExternal.r[0x4C54]=0x21;
   assert(!Playback::commit(wrongInternal,internalTransition,17<<5,true,internal));
   assert(!Playback::commit(wrongExternal,externalTransition,17<<5,true));
   assert(wrongInternal.r[0x6000]==0xA678&&wrongExternal.r[0x6000]==0xA678);
  }
  assert(legacy.r[0x2018]==(dsp?0x3001U:1U)&&hp.r[0x2018]==(dsp?0x3021U:0x21U));
  assert(Playback::commit(legacy,a,17<<5,dsp));
  assert(Playback::commit(hp,c,17<<5,dsp,internal));
  assert(Playback::commit(explicitLegacy,b,17<<5,dsp,kLegacyPowerConfiguration));
  assert(legacy.r==explicitLegacy.r&&legacy.writes==explicitLegacy.writes);
  assert(std::find(legacy.writes.begin(),legacy.writes.end(),0x742c)!=legacy.writes.end());
  assert(std::find(legacy.writes.begin(),legacy.writes.end(),0x11008)!=legacy.writes.end());
  ShutdownTransition sa{},sb{},sc{};ar=br=cr=PlaybackProgress::Pending;
  for(uint64_t now=20;now<40;++now){
   if(ar==PlaybackProgress::Pending)ar=Playback::shutdown(legacy,sa,now,dsp,0x2b01);
   if(cr==PlaybackProgress::Pending)cr=Playback::shutdown(hp,sc,now,dsp,0x2b01,nullptr,internal);
   if(br==PlaybackProgress::Pending)br=Playback::shutdown(explicitLegacy,sb,now,dsp,0x2b01,nullptr,kLegacyPowerConfiguration);
  }
  assert(legacy.r==explicitLegacy.r&&legacy.writes==explicitLegacy.writes);
  assert(legacy.calls==explicitLegacy.calls);
 }
 for(bool dsp:{false,true}){
  Port io;io.r[0x10098]=2;PlaybackTransition playback{};PlaybackProgress result=PlaybackProgress::Pending;
  for(uint64_t now=0;now<20&&result==PlaybackProgress::Pending;++now)
   result=Playback::advance(io,playback,now,dsp,0x2b01,nullptr,internal);
  assert(result==PlaybackProgress::Prepared&&io.r[0x2018]==internal.activePower(dsp));
  assert(Playback::commit(io,playback,17<<5,dsp,internal));
  for(auto reg:io.writes)assert(reg!=0x742c&&reg!=0x7438&&reg!=0x7414&&reg!=0x11008);
  ShutdownTransition shutdown{};result=PlaybackProgress::Pending;
  for(uint64_t now=20;now<40&&result==PlaybackProgress::Pending;++now)
   result=Playback::shutdown(io,shutdown,now,dsp,0x2b01,nullptr,internal);
  assert(result==PlaybackProgress::Prepared&&!(io.r[0x2014]&1)&&!(io.r[0x2018]&0x3031));
  for(auto reg:io.writes)assert(reg!=0x742c&&reg!=0x7438&&reg!=0x7414&&reg!=0x11008);
 }
 Port wrong;wrong.r[0x2014]=1;wrong.r[0x2018]=0x3001;PlaybackTransition prepared{};prepared.phase=PlaybackPhase::Prepared;
 assert(!Playback::commit(wrong,prepared,17<<5,true,internal)&&wrong.writes.empty());
}
''')
    flag_list = [[]] if sys.platform == "win32" else [[], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]]
    for flags in flag_list:
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS CS35L41 legacy/internal power isolation, coefficients, muted preparation, commit and shutdown")
