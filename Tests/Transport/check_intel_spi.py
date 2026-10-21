"""Exercise Intel LPSS FIFO engine and CS35L41 frames against a register model."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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
#include "Transport/SPI/Intel/LPSSSPIEngine.hpp"
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
    unsigned busyTail=0,assertions=0,deassertions=0,failAfter=0,dataReads=0;
    uint32_t rejectOffset=0xffffffffU;
    bool selected=false,stuck=false,forceBusy=false,overrun=false,missing=false,staleForever=false,frozenClock=false;
    bool guardPower=false,blockReset=false,blockClock=false,requireClockUpdate=false,clockApplied=false;
    bool selfClearUpdate=false,corruptClockUpdate=false;
    bool cleanupStale=false;
    bool rejectRelease=false,controlGoneAtFinish=false,errorAfterTwo=false,externalFrame=false;
    size_t writesAtGone=0;
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
        if(o==E::kControl0&&controlGoneAtFinish&&!bytes.empty()){
            writesAtGone=writes.size();return 0xffffffffU;
        }
        if(o==E::kStatus) {
            if(missing || (failAfter && bytes.size()>=failAfter)) return 0xffffffff;
            if(overrun) return E::kOverrun;
            if(errorAfterTwo&&bytes.size()>=2)return E::kOverrun;
            if(forceBusy || (busyTail && busyTail--)) return E::kBusy;
            return E::kTxReady|((!fifo.empty()||staleForever||(cleanupStale&&!selected&&!bytes.empty()))?E::kRxReady:0);
        }
        if(o==E::kData) {
            ++dataReads;
            if(staleForever||(cleanupStale&&!selected&&!bytes.empty())) return 0;
            assert(!fifo.empty()); auto b=fifo.front();fifo.pop_front();return b;
        }
        return regs[o/4];
    }
    void write32(uint32_t o,uint32_t v) override {
        if(guardPower&&o<0x200)assert((regs[E::kReset/4]&3)==3&&(regs[E::kClock/4]&1));
        ++time;writes.emplace_back(o,v);
        if(o==rejectOffset)return;
        if(o==E::kChipSelect&&rejectRelease&&selected&&(v&3)==3)return;
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
            assert((selected||externalFrame) && (regs[E::kControl0/4]&E::kEnabled));
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
struct External : SPIChipSelect {
    Window& w;bool failRelease;unsigned releases=0;
    External(Window& window,bool fail):w(window),failRelease(fail){}
    IOReturn setActive(bool active) override {
        w.externalFrame=active;
        if(active)w.bytes.clear();
        if(!active){++releases;return failRelease?kIOReturnNotPermitted:kIOReturnSuccess;}
        return kIOReturnSuccess;
    }
};
void cleanupRegression(unsigned kind) {
    Controller c;uint8_t tx[10]={0x80},rx[10]{};
    if(kind==0) {
        c.w.rejectRelease=true;
        assert(c.engine.exchange({0,1000000,0},tx,rx,10)==kIOReturnIOError);
        assert(c.engine.firstFailure().cleanupResult==kIOReturnIOError&&!c.engine.online());
        assert(c.engine.firstFailure().primaryResult==0);
        assert(c.engine.firstFailure().cleanupNativeCS==kIOReturnIOError);
    } else if(kind==1) {
        External cs(c.w,true);
        assert(c.engine.exchange({1,1000000,0},tx,rx,10,&cs)==kIOReturnNotPermitted);
        assert(c.engine.firstFailure().cleanupResult==kIOReturnNotPermitted&&cs.releases==1);
        assert(c.engine.firstFailure().cleanupExternalCS==kIOReturnNotPermitted);
        assert(c.engine.firstFailure().cleanupDrain==0);
    } else if(kind==2) {
        c.w.controlGoneAtFinish=true;c.w.errorAfterTwo=true;
        assert(c.engine.exchange({0,1000000,0},tx,rx,10)==kIOReturnIOError);
        assert(c.engine.firstFailure().cleanupResult==kIOReturnNoDevice&&!c.engine.online());
        assert(c.engine.firstFailure().primaryResult==kIOReturnIOError);
        assert(c.engine.firstFailure().cleanupController==kIOReturnNoDevice);
        assert(c.engine.firstFailure().cleanupDrain==kIOReturnNotReady);
        assert(c.w.writes.size()==c.w.writesAtGone);
    } else {
        c.w.failAfter=2;External cs(c.w,true);
        assert(c.engine.exchange({1,1000000,0},tx,rx,10,&cs)==kIOReturnNoDevice);
        assert(c.engine.firstFailure().cleanupResult!=kIOReturnSuccess&&cs.releases==1);
        assert(c.engine.firstFailure().primaryResult==kIOReturnNoDevice);
        assert(c.engine.firstFailure().cleanupController==kIOReturnNoDevice);
        assert(c.engine.firstFailure().cleanupExternalCS==kIOReturnNotPermitted);
    }
}
int main(int argc,char** argv) {
    if(argc==2){cleanupRegression(unsigned(argv[1][0]-'0'));return 0;}
    // Exact identity probes, not arbitrary register scans. Evidence must retain
    // the full ten-byte frame and consume each FIFO byte exactly once.
    for (uint32_t address : {0U,4U}) {
        Controller probe;
        probe.w.codec[address]=address?0xb2:0x35a40;
        uint8_t frame[10]={0x80,0,0,uint8_t(address)},reply[10]{};
        assert(probe.engine.exchange({0,4000000,0},frame,reply,10)==0);
        const auto& e=probe.engine.lastTransaction();
        assert(e.valid&&e.configured&&e.length==10&&e.sent==10&&e.received==10);
        assert(e.tx[0]==0x80&&e.tx[3]==address&&e.rx[9]==(address?0xb2:0x40));
        assert(e.control0==(24U<<8|7U|E::kEnabled)&&e.control1==0&&e.protocol==0&&e.timeout==0);
        assert((e.csInactive&3)==3&&(e.csActive&3)==1&&(e.csFinal&3)==3);
        assert(probe.w.dataReads==10&&!probe.engine.firstFailure().valid);
        assert(probe.engine.firstIdentityTransaction(0,address)->sequence==1);
        assert(!probe.engine.firstIdentityTransaction(0,8));
        assert(!probe.engine.firstIdentityTransaction(4,address));
        auto first=*probe.engine.firstTransaction(0);
        probe.w.stuck=true;
        assert(probe.engine.exchange({0,4000000,0},frame,reply,10)==kIOReturnTimeout);
        auto failure=probe.engine.firstFailure();
        assert(failure.sequence==2&&failure.sent==8&&failure.received==0&&failure.configured);
        probe.w.stuck=false;
        assert(probe.engine.exchange({0,4000000,0},frame,nullptr,10)==0);
        assert(probe.engine.firstFailure().sequence==failure.sequence);
        assert(probe.engine.firstTransaction(0)->sequence==first.sequence);
        assert(probe.engine.lastTransaction().received==10&&probe.engine.lastTransaction().rx[9]==(address?0xb2:0x40));
        assert(probe.engine.firstIdentityTransaction(0,address)->sequence==1);
        assert(!probe.engine.firstTransaction(4));
    }
    // Rejected programming must fail before a single data byte is clocked.
    for(uint32_t offset:{E::kControl0,E::kControl1,0x28U,0x2cU,E::kChipSelect}) {
        Controller fault;
        fault.w.regs[offset/4]=0x55555555;
        fault.w.rejectOffset=offset;
        uint8_t frame[10]={0x80},reply[10]{};
        assert(fault.engine.exchange({0,4000000,0},frame,reply,10)==kIOReturnIOError);
        assert(fault.w.dataReads==0&&fault.w.bytes.empty());
        assert(fault.engine.firstFailure().configured&&fault.engine.firstFailure().sent==0);
    }
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
        // No SSP access when reset/clock readback failed.
        for(auto entry:cold.writes)assert(entry.first>=0x200);
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
    assert(unplug.engine.firstFailure().sent==2&&unplug.engine.firstFailure().received==1);
    assert(unplug.engine.firstFailure().status==0xffffffffU);
    // No diagnostic read consumed the second byte.
    assert(unplug.w.dataReads==1);
    Controller truncated;
    assert(truncated.engine.exchange(ep,tx,rx,32)==0);
    const auto& retained=truncated.engine.lastTransaction();
    assert(retained.sent==32&&retained.received==32&&truncated.w.dataReads==32);
    // A near-match must not be mistaken for a bounded identity read.
    uint8_t other[10]={0x80,0,0,0,0,1};
    assert(truncated.engine.exchange({0,1000000,0},other,rx,10)==0);
    assert(!truncated.engine.firstIdentityTransaction(0,0));
    Controller cleanup;cleanup.w.cleanupStale=true;
    assert(cleanup.engine.exchange(ep,tx,rx,32)==kIOReturnIOError);
    assert(cleanup.engine.firstFailure().sent==32&&cleanup.engine.firstFailure().received==32);
    assert(cleanup.engine.firstFailure().cleanupResult==kIOReturnIOError);
    // Bounded cleanup, not diagnostic reads.
    assert(cleanup.w.dataReads==32+256);
    inactive(cleanup.w);
    Controller primary;primary.w.cleanupStale=true;primary.w.stuck=true;
    assert(primary.engine.exchange(ep,tx,rx,32)==kIOReturnTimeout);
    assert(primary.engine.firstFailure().result==kIOReturnTimeout);
    assert(primary.engine.firstFailure().cleanupResult==kIOReturnIOError);
    // Capture prefixes remain bounded even when the transfer is much larger.
    Controller large;std::vector<uint8_t> largeTX(4096,0x5a);
    assert(large.engine.exchange(ep,largeTX.data(),nullptr,largeTX.size())==0);
    assert(large.engine.lastTransaction().sent==4096&&large.engine.lastTransaction().received==4096);
    assert(large.w.dataReads==4096&&large.engine.lastTransaction().tx[31]==0x5a);
    Controller logical;External external(logical.w,false);uint8_t identityFrame[10]={0x80};
    assert(logical.engine.exchange({0,1000000,0},identityFrame,rx,10)==0);
    const auto identitySequence=logical.engine.firstIdentityTransaction(0,0)->sequence;
    for(unsigned i=0;i<256;++i){
        uint32_t cs=i==255?0xffffffffU:4+i*104729U;
        assert(logical.engine.exchange({cs,1000000,0},identityFrame,rx,10,&external)==0);
        assert(logical.engine.lastTransaction().chipSelect==cs);
        assert(!logical.engine.firstTransaction(cs)&&!logical.engine.firstIdentityTransaction(cs,0));
    }
    assert(external.releases==256);
    assert(logical.engine.firstIdentityTransaction(0,0)->sequence==identitySequence);
    assert(!logical.engine.firstIdentityTransaction(0,4));
}
''')
    for flags in sanitizer_flags():
        exe=tmp/'intel-spi-test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,
                        '-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
                        str(tmp/'test.cpp'),'-o',str(exe)],check=True)
        failures=[]
        for case in range(4):
            result=subprocess.run([str(exe),str(case)],timeout=10)
            if result.returncode:
                failures.append(case)
        assert not failures, f'cleanup regressions failed: {failures}'
        subprocess.run([str(exe)],check=True,timeout=10)
print('PASS Intel LPSS PIO + CS35L41: native CS, full-duplex FIFO, modes, clock, firmware chunks, errors, timeout and suspend; hardware untested')
