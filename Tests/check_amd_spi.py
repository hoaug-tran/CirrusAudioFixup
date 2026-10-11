"""Production AMD command engine with CS35L41 frames and fault injection."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-amd-spi-') as directory:
    p=Path(directory);(p/'IOKit').mkdir()
    (p/'IOKit/IOLib.h').write_text('''#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnNotPermitted=-3,kIOReturnTimeout=-4,kIOReturnBusy=-5,
kIOReturnUnsupported=-6,kIOReturnNoDevice=-7;
inline void IOSleep(unsigned) {}
''')
    (p/'test.cpp').write_text(r'''
#include <cassert>
#include <map>
#include <vector>
#include <cstring>
#include "Transport/AMD/AMDSPIEngine.hpp"
#include "Transport/CS35L41SPITransport.hpp"
using namespace cirrus::transport;
struct Window : amd::RegisterWindow {
    uint8_t regs[0x100]{};uint64_t clock=0;unsigned writes=0,commands=0;
    bool v2=false,stuck=false,gone=false,busy=false;
    std::map<uint32_t,uint32_t> values;
    std::vector<std::vector<uint8_t>> frames;
    uint8_t read8(uint32_t o) override { assert(o<sizeof(regs));return regs[o]; }
    void write8(uint32_t o,uint8_t v) override {assert(o<sizeof(regs));++writes;regs[o]=v;if(o==0x47&&(v&0x80))execute();}
    uint32_t read32(uint32_t o) override {
        assert(o+4<=sizeof(regs));
        if(o==(v2?0x4cU:0U)) {if(gone)return 0xffffffffU;if(busy)return 1U<<31;}
        uint32_t v=0;memcpy(&v,regs+o,4);return v;
    }
    void write32(uint32_t o,uint32_t v) override {
        assert(o+4<=sizeof(regs));++writes;memcpy(regs+o,&v,4);
        if(o==0&&(v&(1<<20)))regs[2]&=~0x10;
        if(!v2&&o==0&&(v&(1<<16))) {regs[2]&=~1;execute();}
    }
    uint64_t nowUs() override {return clock++;}
    void delayUs(uint32_t d) override {clock+=d;}
    void execute() {
        ++commands;busy=stuck;
        const size_t tx=regs[0x48],rx=regs[0x4b];
        std::vector<uint8_t> frame{v2?regs[0x45]:regs[0]};
        frame.insert(frame.end(),regs+0x80,regs+0x80+tx);
        frames.push_back(frame);
        assert(frame.size()>=6 && frame[4]==0 && frame[5]==0);
        uint32_t reg=(uint32_t(frame[0]&0x7f)<<24)|(uint32_t(frame[1])<<16)|(uint32_t(frame[2])<<8)|frame[3];
        if(rx) {
            assert(frame.size()==6&&(frame[0]&0x80));
            for(size_t i=0;i<rx;++i)regs[0x80+tx+i]=uint8_t(values[reg+uint32_t(i/4)*4]>>(24-8*(i%4)));
        } else {
            assert(!(frame[0]&0x80));
            for(size_t i=6;i<frame.size();i+=4)values[reg+uint32_t(i-6)]=(uint32_t(frame[i])<<24)|(uint32_t(frame[i+1])<<16)|(uint32_t(frame[i+2])<<8)|frame[i+3];
        }
    }
};
int main() {
 for(bool v2:{false,true}) {
    Window w;w.v2=v2;amd::AMDSPIEngine bus(w);
    auto rev=v2?amd::AMDSPIRevision::V2:amd::AMDSPIRevision::V1;
    assert(bus.initialize(rev)==0);
    SPIEndpoint ep{1,1000000,0};CS35L41SPITransport io(&bus,ep,true);
    w.values[0]=0x35a40;uint32_t id=0;
    assert(io.read(0,&id)&&id==0x35a40&&bus.actualClockHz()==800000);
    assert(io.write(0x2018,0x11223344)&&w.values[0x2018]==0x11223344);
    assert(io.updateBits(0x2018,0xff,0xaa)&&w.values[0x2018]==0x112233aa);
    uint8_t payload[520];for(unsigned i=0;i<sizeof(payload);++i)payload[i]=uint8_t(i);
    unsigned before=w.commands;
    assert(io.bulkWrite(0x2800000,payload,sizeof(payload))&&w.commands-before==10);
    uint8_t output[520]{};assert(io.bulkRead(0x2800000,output,sizeof(output))&&!memcmp(payload,output,sizeof(payload)));
    uint8_t head[6]={0x80};uint8_t rx[4];
    unsigned n=w.writes;
    assert(bus.command({0,799999,0},head,6,rx,4)==kIOReturnUnsupported&&w.writes==n);
    assert(bus.command({0,1000000,1},head,6,rx,4)==kIOReturnUnsupported&&w.writes==n);
    assert(bus.command({4,1000000,0},head,6,rx,4)==kIOReturnBadArgument&&w.writes==n);
    assert(bus.command(ep,head,6,rx,59)==kIOReturnBadArgument&&w.writes==n);
    assert(bus.exchange(ep,head,rx,4)==kIOReturnUnsupported);
    assert(bus.command({0,100000000,0},head,6,rx,4)==0&&(w.read32(0x20)&1));
    assert(bus.command(ep,head,6,rx,4)==0&&!(w.read32(0x20)&1));
    bus.suspend();n=w.writes;assert(!io.read(0,&id)&&w.writes==n);
    assert(bus.resume()==0&&io.read(0,&id));
    w.busy=true;n=w.writes;assert(!io.read(0,&id)&&w.writes==n);w.busy=false;
    w.stuck=true;id=0xdead;assert(!io.read(0,&id)&&id==0xdead&&io.lastReturn()==kIOReturnTimeout);
    assert(bus.resume()==kIOReturnBusy);n=w.writes;bus.shutdown();assert(w.writes==n);
    w.busy=false;w.stuck=false;assert(bus.initialize(rev)==0);
    w.gone=true;n=w.writes;assert(!io.read(0,&id)&&w.writes==n&&io.lastReturn()==kIOReturnNoDevice);
    bus.abandon();
 }
 Window clean;amd::AMDSPIEngine bus(clean);assert(bus.initialize(amd::AMDSPIRevision::V1)==0);
 CS35L41SPITransport io(&bus,{0,4000000,0},true);assert(io.write(0x100,1));bus.shutdown();
 assert(clean.read32(0x20)==0&&clean.read32(0x6c)==0&&clean.read8(0x1d)==0);
}
''')
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
print('PASS AMD V1/V2 command FIFO: CS35L41 bulk/RMW, clocks, limits, timeout and power gating; hardware untested')
