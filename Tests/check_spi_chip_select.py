from host_build import sanitizer_flags
"""Replay mixed native/GPIO CS through the production LPSS FIFO engine."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cirrus-mixed-cs-") as directory:
    tmp = Path(directory)
    (tmp / "IOKit").mkdir()
    (tmp / "IOKit/IOLib.h").write_text("""#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnUnsupported=-3,kIOReturnBusy=-4,kIOReturnNoDevice=-5,
kIOReturnIOError=-6,kIOReturnTimeout=-7,kIOReturnNotPermitted=-8;
""")
    (tmp / "test.cpp").write_text(r'''
#include <cassert>
#include <deque>
#include <vector>
#include "Transport/Intel/LPSSSPIEngine.hpp"
using namespace cirrus::transport;
using namespace cirrus::transport::intel;
using E=LPSSSPIEngine;
struct Window:RegisterWindow {
 uint32_t regs[0x300/4]{};uint64_t time=0;
 std::deque<uint8_t> fifo;std::vector<uint8_t> sent;
 bool gpio=false,native=false,stuck=false,gone=false,sawGone=false;unsigned nativeAssertions=0,statusReads=0,goneAt=0;
 Window(){regs[E::kCapabilities/4]=0x220;regs[E::kChipSelect/4]=3;}
 uint32_t read32(uint32_t o)override{
  ++time;
  if(o==E::kStatus){++statusReads;if(gone||(goneAt&&statusReads>=goneAt)){sawGone=true;return 0xffffffff;}
   return E::kTxReady|(fifo.empty()?0:E::kRxReady);}
  if(o==E::kData){auto v=fifo.front();fifo.pop_front();return v;}return regs[o/4];
 }
 void write32(uint32_t o,uint32_t v)override{
  assert(!sawGone);
  if(o==E::kChipSelect){native=(v&3)==1;if(native)++nativeAssertions;}
  if(o==E::kData){assert((native!=gpio)&&(regs[E::kControl0/4]&E::kEnabled));
   sent.push_back(v);if(!stuck)fifo.push_back(v);}
  if(o==E::kControl0&&!(v&E::kEnabled))fifo.clear();
  regs[o/4]=v;
 }
 uint64_t nowUs()override{return ++time;}void delayUs(uint32_t n)override{time+=n;}
};
struct Select:SPIChipSelect {
 Window& w;std::vector<bool> calls;bool failAssert=false,failRelease=false;IOReturn assertError=kIOReturnIOError;
 explicit Select(Window& v):w(v){}
 IOReturn setActive(bool active)override{
  assert(!w.native);calls.push_back(active);w.gpio=active;
  return active&&failAssert?assertError:(!active&&failRelease?kIOReturnIOError:0);
 }
};
int main(){
 uint8_t tx[10]={0x80,0,0,0,0,0,0,0,0,0},rx[10]{};
 // The HP capture has one native CS plus a second GPIO CS. Logical CS1
 // must be usable even when capabilities only advertise native CS0.
 Window w;E e(w);Select s(w);assert(e.initialize()==0);
 assert(e.exchange({1,4000000,0},tx,rx,10,&s)==0);
 assert(w.sent.size()==10&&w.nativeAssertions==0&&!w.gpio);
 assert(s.calls==std::vector<bool>({true,false}));
 assert(e.exchange({0,4000000,0},tx,rx,10)==0&&w.nativeAssertions==1);
 Window stuck;E timeout(stuck);Select ts(stuck);assert(timeout.initialize()==0);stuck.stuck=true;
 assert(timeout.exchange({9,4000000,0},tx,rx,10,&ts)==kIOReturnTimeout);
 assert(!stuck.gpio&&!stuck.native&&ts.calls.size()==2);
 Window failed;E f(failed);Select fs(failed);assert(f.initialize()==0);fs.failAssert=true;
 assert(f.exchange({1,4000000,0},tx,rx,10,&fs)==kIOReturnIOError);
 assert(failed.sent.empty()&&!failed.gpio&&fs.calls.size()==2);
 Window gpioGone;E g(gpioGone);Select gs(gpioGone);assert(g.initialize()==0);gs.failAssert=true;gs.assertError=kIOReturnNoDevice;
 assert(g.exchange({1,4000000,0},tx,rx,10,&gs)==kIOReturnNoDevice);
 assert(!(gpioGone.regs[E::kControl0/4]&E::kEnabled)&&!gpioGone.gpio&&gpioGone.sent.empty());
 Window release;E r(release);Select rs(release);assert(r.initialize()==0);rs.failRelease=true;
 assert(r.exchange({1,4000000,0},tx,rx,10,&rs)==kIOReturnIOError&&!r.online());
 Window drainGone;E d(drainGone);Select ds(drainGone);assert(d.initialize()==0);drainGone.goneAt=3;
 assert(d.exchange({1,4000000,0},tx,rx,10,&ds)==kIOReturnNoDevice&&!d.online());
 assert(drainGone.sawGone&&ds.calls.empty());
 // Invalid and suspended requests never toggle a GPIO.
 auto n=s.calls.size();assert(e.exchange({1,0,0},tx,rx,10,&s)==kIOReturnBadArgument);
 e.suspend();assert(e.exchange({1,4000000,0},tx,rx,10,&s)==kIOReturnNotReady&&s.calls.size()==n);
}
''')
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS mixed native/GPIO CS: no native assertion on GPIO frames, bounded FIFO, cleanup and PM")
