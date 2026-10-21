"""Exercise production CS35L41 SPI frames, chunking and failures without hardware."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='cirrus-spi-') as directory:
    tmp = Path(directory)
    (tmp/'IOKit').mkdir()
    (tmp/'IOKit/IOLib.h').write_text('''#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
              kIOReturnNotPermitted=-3,kIOReturnTimeout=-4;
inline unsigned sleeps=0;
inline void IOSleep(unsigned) { ++sleeps; }
''')
    (tmp/'test.cpp').write_text(r'''
#include <cassert>
#include <vector>
#include "Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp"
using namespace cirrus::transport;
struct Controller : SPIController {
    size_t limit=262;
    unsigned calls=0,failAt=0;
    uint32_t value=0x11223344;
    std::vector<std::vector<uint8_t>> frames;
    size_t maxTransferBytes() const override { return limit; }
    IOReturn exchange(const SPIEndpoint& ep,const uint8_t* tx,uint8_t* rx,size_t n) override {
        assert(ep.chipSelect==1 && ep.clockHz==1000000 && ep.mode==0);
        assert(n<=limit && n>=10 && tx[4]==0 && tx[5]==0);
        assert(bool(rx)==bool(tx[0]&0x80));
        frames.emplace_back(tx,tx+n);
        if (++calls==failAt) return -77;
        if(rx) {
            for(size_t i=0;i<n;++i) rx[i]=0xee;
            for(size_t i=6;i<n;++i) {
                assert(tx[i]==0);
                rx[i]=uint8_t(value>>(24-8*((i-6)%4)));
            }
        }
        return 0;
    }
};
int main() {
    SPIEndpoint ep{1,1000000,0};
    Controller c;
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO io(&c,ep,true);
    uint32_t value=0;
    assert(io.read(0x02800398,&value) && value==0x11223344);
    assert(c.frames.back()==std::vector<uint8_t>({0x82,0x80,3,0x98,0,0,0,0,0,0}));
    assert(io.write(0x2018,0x12345678));
    assert(c.frames.back()==std::vector<uint8_t>({0,0,0x20,0x18,0,0,0x12,0x34,0x56,0x78}));
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO ro(&c,ep);
    unsigned before=c.calls;
    assert(!ro.write(0x2018,0) && ro.lastReturn()==kIOReturnNotPermitted && c.calls==before);
    assert(ro.updateBits(0x2018,0,0) && c.calls==before+1);
    assert(!ro.updateBits(0x2018,0xff,0x55) && ro.lastReturn()==kIOReturnNotPermitted);
    before=c.calls;
    assert(io.updateBits(0x2018,0xff,0x55) && c.calls==before+2);
    assert(c.frames.back()[9]==0x55 && c.frames.back()[6]==0x11);
    std::vector<uint8_t> data(520);
    for(size_t i=0;i<data.size();++i) data[i]=uint8_t(i);
    c.frames.clear();
    assert(io.bulkWrite(0x02800000,data.data(),data.size()));
    assert(c.frames.size()==3 && c.frames[0].size()==262 && c.frames[2].size()==14);
    assert(c.frames[1][2]==1 && c.frames[2][2]==2);
    for(size_t i=0;i<256;++i) assert(c.frames[0][i+6]==data[i]);
    c.limit=19; c.frames.clear();
    assert(io.bulkRead(0x100,data.data(),20));
    assert(c.frames.size()==2 && c.frames[0].size()==18 && c.frames[1][3]==0x0c);
    assert(data[0]==0x11 && data[12]==0x11 && data[19]==0x44);
    before=c.calls;
    // Linux CS35L41_LASTREG + 4
    assert(!io.write(0x03804fec,0));
    assert(!io.bulkWrite(0x03804fe8,data.data(),8));
    // Reject the entire range before any transfer.
    assert(c.calls==before);
    assert(!io.bulkWrite(0x7ffffffc,data.data(),8));
    assert(!io.bulkRead(1,data.data(),4));
    assert(!io.bulkRead(0,data.data(),3));
    assert(!io.bulkRead(0,nullptr,4));
    assert(!io.read(0,nullptr));
    assert(c.calls==before && io.lastReturn()==kIOReturnBadArgument);
    c.limit=9;
    assert(!io.read(0,&value) && c.calls==before && io.lastReturn()==kIOReturnNotReady);
    c.limit=262;c.failAt=c.calls+1;value=0xcafebabe;
    assert(!io.read(0,&value) && value==0xcafebabe && io.lastReturn()==-77);
    c.failAt=c.calls+2;before=c.calls;
    assert(!io.bulkWrite(0,data.data(),data.size()) && c.calls==before+2 && io.lastReturn()==-77);
    c.failAt=0;
    assert(io.pollBit(0,0xff,0x44,5));
    before=c.calls;sleeps=0;
    assert(!io.pollBit(0,0xff,0x55,3) && io.lastReturn()==kIOReturnTimeout);
    assert(c.calls==before+3 && sleeps==2);
    c.failAt=c.calls+1;before=c.calls;
    assert(!io.pollBit(0,1,1,5) && c.calls==before+1 && io.lastReturn()==-77);
    before=c.calls;
    assert(!io.pollBit(0,1,2,5) && !io.pollBit(0,1,1,0) && c.calls==before);
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO missing(nullptr,ep);
    assert(!missing.read(0,&value) && missing.lastReturn()==kIOReturnNotReady);
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO badClock(&c,{1,0,0});
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO badMode(&c,{1,1000000,4});
    assert(!badClock.read(0,&value) && !badMode.read(0,&value) && c.calls==before);
}
''')
    for flags in sanitizer_flags():
        exe = tmp/'spi-test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,
                        '-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
                        str(tmp/'test.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('PASS CS35L41 SPI framing, endian, limits, read-only policy, RMW, timeout and failure propagation; physical controller untested')
