from host_build import sanitizer_flags
"""Exercise Intel LPSS FIFO engine and CS35L41 frames against a register model."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-intel-spi-') as directory:
    tmp = Path(directory)
    (tmp/'IOKit').mkdir()
    (tmp/'IOKit/IOLib.h').write_text('''#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
 kIOReturnUnsupported=-3,kIOReturnBusy=-4,kIOReturnNoDevice=-5,
 kIOReturnIOError=-6,kIOReturnTimeout=-7,kIOReturnNotPermitted=-8;
inline void IOSleep(unsigned) {}
''')
    (tmp/'test.cpp').write_text(r'''
#include <cassert>
#include <deque>
#include <vector>
#include <map>
#include "Transport/Intel/LPSSSPIEngine.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41Device.hpp"
bool gCirrusDebug=false;
using namespace cirrus::transport;
using namespace cirrus::transport::intel;
using E=LPSSSPIEngine;
struct Window : RegisterWindow {
    uint32_t regs[0x300/4]{};
    uint64_t time=0;
    std::deque<uint8_t> fifo;
    std::vector<uint8_t> bytes;
    std::map<uint32_t,uint32_t> codec;
    std::vector<std::vector<uint8_t>> transactions;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    unsigned busyTail=0,assertions=0,deassertions=0,failAfter=0;
    bool selected=false,stuck=false,forceBusy=false,overrun=false,missing=false,staleForever=false,frozenClock=false;
    bool guardPower=false,blockReset=false,blockClock=false,requireClockUpdate=false,clockApplied=false;
    bool selfClearUpdate=false,corruptClockUpdate=false;
    Window() {
        regs[E::kCapabilities/4]=0x20|(3<<9);
        regs[E::kChipSelect/4]=3;
        regs[E::kClock/4]=0x80000000U|1|(1<<1)|(1<<16);
        regs[E::kClockGate/4]=0x80;
        regs[E::kReset/4]=7;
    }
    uint32_t read32(uint32_t o) override {
        if(requireClockUpdate&&o<0x200)assert(clockApplied);
        if(guardPower&&o<0x200)assert((regs[E::kReset/4]&3)==3&&(regs[E::kClock/4]&1));
        ++time;
        if(o==E::kStatus) {
            if(missing || (failAfter && bytes.size()>=failAfter)) return 0xffffffff;
            if(overrun) return E::kOverrun;
            if(forceBusy || (busyTail && busyTail--)) return E::kBusy;
            return E::kTxReady|((!fifo.empty()||staleForever)?E::kRxReady:0);
        }
        if(o==E::kData) {
            if(staleForever) return 0;
            assert(!fifo.empty()); auto b=fifo.front();fifo.pop_front();return b;
        }
        return regs[o/4];
    }
    void write32(uint32_t o,uint32_t v) override {
        if(guardPower&&o<0x200)assert((regs[E::kReset/4]&3)==3&&(regs[E::kClock/4]&1));
        ++time;writes.emplace_back(o,v);
        if((blockReset&&o==E::kReset)||(blockClock&&o==E::kClock))return;
        if(o==E::kStatus) return;
        if(o==E::kChipSelect) {
            bool next=(v&3)==1;
            if(next&&!selected) {assertions++;bytes.clear();}
            if(!next&&selected) {
                deassertions++;transactions.push_back(bytes);
                if(bytes.size()>=10 && !(bytes[0]&0x80)) {
                    uint32_t address=uint32_t(bytes[0])<<24|uint32_t(bytes[1])<<16|uint32_t(bytes[2])<<8|bytes[3];
                    for(size_t n=6;n+3<bytes.size();n+=4)
                        codec[address+uint32_t(n-6)]=uint32_t(bytes[n])<<24|uint32_t(bytes[n+1])<<16|uint32_t(bytes[n+2])<<8|bytes[n+3];
                }
            }
            selected=next;
        }
        if(o==E::kControl0 && !(v&E::kEnabled)) fifo.clear();
        regs[o/4]=v;
        if(o==E::kClock&&(v&0x80000000U)){
            clockApplied=true;
            if(selfClearUpdate)regs[o/4]&=0x7fffffffU;
            if(corruptClockUpdate)regs[o/4]^=2U;
        }
        if(o==E::kData) {
            assert(selected && (regs[E::kControl0/4]&E::kEnabled));
            bytes.push_back(uint8_t(v));
            if(!stuck) {
                // Return recognizable data after the six-byte CS35L41 header.
                auto n=bytes.size();uint8_t b=0xee;
                if(n>6) {
                    uint32_t address=uint32_t(bytes[0]&0x7f)<<24|uint32_t(bytes[1])<<16|uint32_t(bytes[2])<<8|bytes[3];
                    address+=uint32_t((n-7)/4*4);
                    auto found=codec.find(address);
                    uint32_t value=found==codec.end()?0x11223344:found->second;
                    b=uint8_t(value>>(24-8*((n-7)%4)));
                }
                fifo.push_back(b);
            }
        }
    }
    uint64_t nowUs() override { return frozenClock?0:++time; }
    void delayUs(uint32_t v) override { time+=v; }
};
struct Controller : SPIController {
    Window w; E engine{w};
    Controller() { assert(engine.initialize()==0); }
    size_t maxTransferBytes() const override { return E::kMaxFrameBytes; }
    IOReturn exchange(const SPIEndpoint& ep,const uint8_t* tx,uint8_t* rx,size_t n) override { return engine.exchange(ep,tx,rx,n); }
};
void inactive(const Window& w) {
    assert(!w.selected && !(w.regs[E::kControl0/4]&E::kEnabled));
    assert(w.regs[E::kClockGate/4]==0x80);
}
int main() {
    { Window captured;captured.requireClockUpdate=true;
      captured.regs[E::kClock/4]=0x7fff0a3d;
      E engine(captured);assert(engine.initialize()==0&&captured.clockApplied);
      assert((captured.regs[E::kClock/4]&0x7ffffffeU)==0x7fff0a3c);
      engine.suspend();captured.clockApplied=false;captured.regs[E::kClock/4]&=0x7fffffffU;
      assert(engine.resume()==0&&captured.clockApplied);engine.shutdown();
      assert(captured.regs[E::kClock/4]==0x7fff0a3d); }
    { Window pulse;pulse.requireClockUpdate=true;pulse.selfClearUpdate=true;
      pulse.regs[E::kClock/4]=0x7fff0a3d;E engine(pulse);
      assert(engine.initialize()==0&&engine.preparedClock()==0x7fff0a3d);engine.shutdown(); }
    { Window corrupt;corrupt.requireClockUpdate=true;corrupt.corruptClockUpdate=true;
      corrupt.regs[E::kClock/4]=0x7fff0a3d;E engine(corrupt);
      assert(engine.initialize()==kIOReturnNotReady&&!engine.online());
      assert(corrupt.regs[E::kClock/4]==0x7fff0a3d); }
    {
        Window cold;cold.guardPower=true;cold.regs[E::kReset/4]=4;cold.regs[E::kClock/4]=0;
        E engine(cold);assert(engine.initialize()==0);
        assert(cold.regs[E::kReset/4]==7&&(cold.regs[E::kClock/4]&1));
        engine.suspend();cold.regs[E::kReset/4]=4;cold.regs[E::kClock/4]=0;
        assert(engine.resume()==0&&cold.regs[E::kReset/4]==7&&(cold.regs[E::kClock/4]&1));
        cold.regs[E::kReset/4]=4;uint8_t tx[10]{},rx[10]{};
        assert(engine.exchange({0,1000000,0},tx,rx,10)==kIOReturnNotReady&&!engine.online());
        engine.shutdown();assert(cold.regs[E::kReset/4]==4&&cold.regs[E::kClock/4]==0);
    }
    for(bool clockFailure:{false,true}){
        Window cold;cold.guardPower=true;cold.blockReset=!clockFailure;cold.blockClock=clockFailure;
        cold.regs[E::kReset/4]=4;cold.regs[E::kClock/4]=0;
        E engine(cold);assert(engine.initialize()==kIOReturnNotReady&&!engine.online());
        for(auto entry:cold.writes)assert(entry.first>=0x200); // No SSP access when reset/clock readback failed.
        assert(cold.regs[E::kReset/4]==4&&cold.regs[E::kClock/4]==0);
    }
    for (uint16_t id : {uint16_t(0x9d29),uint16_t(0x1ac2),uint16_t(0xa32a),uint16_t(0x51aa),uint16_t(0x7aaa)}) {
        LPSSSPIProfile profile{}; assert(lookupLPSSSPIProfile(0x8086,id,profile));
        Window w; E engine(w); assert(engine.initialize(profile)==0);
        uint8_t txProfile[10]{},rxProfile[10]{};
        assert(engine.exchange({0,7000000,0},txProfile,rxProfile,10)==0);
        uint32_t divisor=(profile.rootClockHz+6999999)/7000000;
        assert(engine.actualClockHz()==profile.rootClockHz/divisor);
        assert(((w.regs[E::kControl0/4]>>8)&0xfff)==divisor-1);
        assert(engine.selectCount()==(profile.capabilities?2U:1U));
        if(!profile.capabilities) assert(engine.exchange({1,1000000,0},txProfile,rxProfile,10)==kIOReturnBadArgument);
        engine.shutdown();
    }
    LPSSSPIProfile rejected{};
    assert(!lookupLPSSSPIProfile(0x1022,0x7aaa,rejected));
    assert(!lookupLPSSSPIProfile(0x8086,0xa123,rejected));

    SPIEndpoint ep{1,1000000,0}; uint8_t tx[32]{},rx[32]{};
    Controller c; assert(c.w.writes.empty() && c.engine.selectCount()==2);
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO amp(&c,ep,true);
    uint32_t value=0;
    assert(amp.read(0x02800398,&value)&&value==0x11223344);
    assert(c.w.transactions.back()==std::vector<uint8_t>({0x82,0x80,3,0x98,0,0,0,0,0,0}));
    assert(amp.write(0x2018,0x12345678));
    assert(c.w.transactions.back()==std::vector<uint8_t>({0,0,0x20,0x18,0,0,0x12,0x34,0x56,0x78}));
    assert(c.w.assertions==2&&c.w.deassertions==2);inactive(c.w);
    std::vector<uint8_t> firmware(520,0x5a);
    assert(amp.bulkWrite(0x02800000,firmware.data(),firmware.size()));
    assert(c.w.transactions.size()==5&&c.w.transactions[2].size()==262);
    for(unsigned mode=0;mode<4;++mode) {
        assert(c.engine.exchange({0,3000000,uint8_t(mode)},tx,rx,32)==0);
        assert(c.engine.actualClockHz()<=3000000);
        assert(c.w.regs[E::kControl1/4]==(((mode&2)?8:0)|((mode&1)?16:0)));
        assert((c.w.regs[E::kChipSelect/4]&0x300)==0);inactive(c.w);
    }
    c.w.regs[E::kClock/4]=1|(1<<1)|(2<<16);
    assert(c.engine.exchange(ep,tx,rx,32)==0&&c.engine.actualClockHz()==1000000);
    assert(((c.w.regs[E::kControl0/4]>>8)&0xfff)==49);
    c.engine.suspend();auto count=c.w.writes.size();
    assert(c.engine.exchange(ep,tx,rx,32)==kIOReturnNotReady&&c.w.writes.size()==count);
    assert(c.engine.resume()==0);assert(c.engine.exchange(ep,tx,nullptr,32)==0);
    c.engine.shutdown();count=c.w.writes.size();
    assert(c.engine.exchange(ep,tx,rx,32)==kIOReturnNotReady&&c.w.writes.size()==count);
    // Existing silicon logic consumes RegisterIO unchanged on an SPI driver.
    Controller logic;
    namespace registers=cirrus::devices::amplifiers::cs35l41::registers;
    logic.w.codec[registers::kRegDeviceId]=registers::kValDeviceId;
    logic.w.codec[registers::kRegRevisionId]=0xb2;
    cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO spiIO(&logic,{0,1000000,0},true);
    cirrus::core::BusEndpoint identity{cirrus::core::BusTransportType::SPI,"\\_SB.SPI1",0,false,1,1000000,0};
    cirrus::devices::amplifiers::cs35l41::CS35L41Device device("left",identity);
    assert(device.address()==0xff&&device.endpoint().sameDevice(identity));
    assert(device.probe(spiIO)&&device.isPresent());
    bool locked=false;
    assert(device.setupPLL(spiIO,&locked));
    assert(device.setupASP(spiIO,false,false));
    assert(device.setupGPIO(spiIO));
    inactive(logic.w);
    Controller invalid;count=invalid.w.writes.size();
    assert(invalid.engine.exchange({2,1000000,0},tx,rx,32)==kIOReturnBadArgument);
    assert(invalid.engine.exchange({0,0,0},tx,rx,32)==kIOReturnBadArgument);
    assert(invalid.engine.exchange({0,1000000,4},tx,rx,32)==kIOReturnBadArgument);
    assert(invalid.engine.exchange(ep,nullptr,rx,32)==kIOReturnBadArgument);
    assert(invalid.engine.exchange(ep,tx,rx,4097)==kIOReturnBadArgument);
    assert(invalid.w.writes.size()==count);
    invalid.w.regs[E::kClock/4]=1|(1<<1);
    assert(invalid.engine.exchange(ep,tx,rx,32)==kIOReturnUnsupported&&invalid.w.writes.size()==count);
    Controller lowClock;
    assert(lowClock.engine.exchange({0,1,0},tx,rx,32)==kIOReturnUnsupported&&lowClock.w.writes.empty());
    for(unsigned kind=0;kind<3;++kind) {
        Window w;
        if(kind==0)w.regs[E::kCapabilities/4]=0xffffffff;
        if(kind==1)w.regs[E::kCapabilities/4]=0x20|(5<<9);
        if(kind==2)w.forceBusy=true;
        E engine(w);assert(engine.initialize()!=0&&w.writes.empty());
    }
    Controller timeout;timeout.w.stuck=true;
    assert(timeout.engine.exchange(ep,tx,rx,32)==kIOReturnTimeout);inactive(timeout.w);
    assert(timeout.w.assertions==1&&timeout.w.deassertions==1);
    timeout.w.stuck=false;assert(timeout.engine.exchange(ep,tx,rx,32)==0);inactive(timeout.w);
    Controller frozen;frozen.w.stuck=true;frozen.w.frozenClock=true;
    assert(frozen.engine.exchange(ep,tx,rx,32)==kIOReturnTimeout);inactive(frozen.w);
    Controller errors;errors.w.overrun=true;
    assert(errors.engine.exchange(ep,tx,rx,32)==kIOReturnIOError);inactive(errors.w);
    errors.w.overrun=false;errors.w.staleForever=true;
    assert(errors.engine.exchange(ep,tx,rx,32)==kIOReturnIOError);inactive(errors.w);
    Controller busy;busy.w.forceBusy=true;count=busy.w.writes.size();
    assert(busy.engine.exchange(ep,tx,rx,32)==kIOReturnBusy&&busy.w.writes.size()==count);
    Controller gone;gone.w.missing=true;
    assert(gone.engine.exchange(ep,tx,rx,32)==kIOReturnNoDevice&&gone.w.writes.empty());
    Controller unplug;unplug.w.failAfter=2;
    assert(unplug.engine.exchange(ep,tx,rx,32)==kIOReturnNoDevice&&!unplug.engine.online());
}
''')
    for flags in sanitizer_flags():
        exe=tmp/'intel-spi-test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,
                        '-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
                        str(tmp/'test.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)
print('PASS Intel LPSS PIO + CS35L41: native CS, full-duplex FIFO, modes, clock, firmware chunks, errors, timeout and suspend; hardware untested')
