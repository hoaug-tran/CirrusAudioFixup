"""Internal/external CS35L41 power sequences with real playback state machines."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cirrus-power-") as directory:
    tmp = Path(directory)
    (tmp / "test.cpp").write_text(r'''
#include <cassert>
#include <map>
#include <vector>
#include "Devices/Amplifiers/CS35L41/Playback.hpp"
using namespace cirrus::devices::amplifiers::cs35l41;
struct Port:cirrus::core::RegisterIO {
 std::map<uint32_t,uint32_t> r;std::vector<uint32_t> writes;unsigned calls=0,failAt=0;
 bool step(){return ++calls!=failAt;}
 bool read(uint32_t reg,uint32_t* out)override{if(!step())return false;*out=r[reg];return true;}
 bool write(uint32_t reg,uint32_t value)override{
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
 // Commit must reject the old external-boost invariant for an internal board.
 Port wrong;wrong.r[0x2014]=1;wrong.r[0x2018]=0x3001;PlaybackTransition prepared{};prepared.phase=PlaybackPhase::Prepared;
 assert(!Playback::commit(wrong,prepared,17<<5,true,internal)&&wrong.writes.empty());
}
''')
    for flags in ([], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(ROOT / "CirrusAudioFixup"), str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS CS35L41 internal boost coefficients, active power, muted preparation, commit and shutdown")
