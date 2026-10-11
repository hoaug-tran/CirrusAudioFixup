"""Execute the production PCI SPI owner against IOKit lifecycle mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-spi-pci-') as directory:
    tmp=Path(directory)
    for name in ['IOKit/pci','kern','mach']:
        (tmp/name).mkdir(parents=True,exist_ok=True)
    (tmp/'IOKit/IOLib.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cassert>
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
 kIOReturnUnsupported=-3,kIOReturnBusy=-4,kIOReturnNoDevice=-5,
 kIOReturnIOError=-6,kIOReturnTimeout=-7,kIOReturnNotPermitted=-8,
 kIOReturnNoMemory=-9,kIOReturnExclusiveAccess=-10,kIOReturnNoResources=-11;
inline uint64_t fakeNs=0;
inline bool allocationFail=false;
struct IOLock {bool held=false;};
inline IOLock lockStorage;
inline unsigned allocs=0,frees=0;
inline IOLock* IOLockAlloc() {if(allocationFail)return nullptr;++allocs;return &lockStorage;}
inline void IOLockFree(IOLock* l) {assert(!l->held);++frees;}
inline void IOLockLock(IOLock* l) {assert(!l->held);l->held=true;}
inline void IOLockUnlock(IOLock* l) {assert(l->held);l->held=false;}
inline void IODelay(unsigned us) {fakeNs+=uint64_t(us)*1000;}
inline void IOSleep(unsigned ms) {fakeNs+=uint64_t(ms)*1000000;}
inline void OSSynchronizeIO() {}
''')
    (tmp/'IOKit/IOService.h').write_text(r'''
#pragma once
#include "IOLib.h"
class IOService {};
struct IOMemoryMap {
    uint32_t regs[0x300/4]{};unsigned releases=0;bool shortMap=false;
    IOMemoryMap(){regs[0x2fc/4]=0x20|(3<<9);regs[0x224/4]=3;}
    uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(regs);}
    size_t getLength(){return shortMap?0x100:sizeof(regs);}
    void release(){++releases;}
};
'''.replace('#include "IOLib.h"','#include "IOLib.h"\n#include <cstddef>'))
    (tmp/'IOKit/pci/IOPCIDevice.h').write_text(r'''
#pragma once
#include "IOKit/IOService.h"
constexpr uint8_t kIOPCIPowerManagementCapability=1,kIOPCIConfigCommand=4,kIOPCIConfigBaseAddress0=0x10;
struct IOPCIDevice {
    unsigned refs=1,opens=0,closes=0;
    bool opened=false,rejectOpen=false,rejectMap=false,rejectMemory=false,memory=false;
    uint16_t vendor=0x8086,device=0x7aaa,state=0;
    IOMemoryMap map;
    IOService* client=nullptr;
    uint16_t configRead16(uint8_t o){return o==0?vendor:o==2?device:o==4?(memory?2:0):o==0x44?state:0;}
    void retain(){++refs;}void release(){--refs;}
    bool isOpen(){return opened;}
    bool open(IOService* c){++opens;if(rejectOpen)return false;opened=true;client=c;return true;}
    void close(IOService* c){assert(opened&&client==c);opened=false;++closes;}
    uint32_t findPCICapability(uint8_t,uint8_t* out){*out=0x40;return 1;}
    bool setMemoryEnable(bool b){if(!rejectMemory)memory=b;return memory;}
    IOMemoryMap* mapDeviceMemoryWithRegister(uint8_t reg){assert(reg==0x10);return rejectMap?nullptr:&map;}
};
''')
    (tmp/'kern/clock.h').write_text(r'''
#pragma once
#include "IOKit/IOLib.h"
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* ns){*ns=t;}
''')
    (tmp/'mach/mach_time.h').write_text(r'''
#pragma once
#include "IOKit/IOLib.h"
inline uint64_t mach_absolute_time(){fakeNs+=1000;return fakeNs;}
''')
    (tmp/'test.cpp').write_text(r'''
#include <cstring>
#include "Transport/Intel/IntelLPSSSPIController.hpp"
using namespace cirrus::transport;
using namespace cirrus::transport::intel;
int main(){
    IOService owner;SPIEndpoint ep{0,1000000,0};uint8_t tx[10]{},rx[10]{};
    IOPCIDevice pci;
    {
        IntelLPSSSPIController bus;
        assert(bus.exchange(ep,tx,rx,10)==kIOReturnNotReady);
        assert(bus.bind(&pci,&owner)==0&&pci.refs==2&&pci.opened&&pci.memory);
        assert(bus.bind(&pci,&owner)==kIOReturnBusy);
        auto clock=pci.map.regs[0x200/4];
        bus.suspend();
        assert(bus.exchange(ep,tx,rx,10)==kIOReturnNotReady&&pci.map.regs[0x200/4]==clock);
        pci.state=3;assert(bus.resume()==kIOReturnNotReady);
        bus.suspend();assert(bus.exchange(ep,tx,rx,10)==kIOReturnNotReady);
        pci.state=0;assert(bus.resume()==0);
        bus.shutdown();assert(pci.refs==1&&!pci.opened&&!pci.memory&&pci.map.releases==1);
        bus.shutdown();assert(pci.closes==1);
        assert(bus.bind(&pci,&owner)==0); // owner can bind again after teardown
    }
    assert(pci.refs==1&&!pci.opened&&pci.closes==2&&allocs==frees);
    for(unsigned mode=0;mode<7;++mode){
        IOPCIDevice bad;IntelLPSSSPIController bus;
        if(mode==0)bad.device=0x1234;
        if(mode==1)bad.opened=true;
        if(mode==2)bad.rejectOpen=true;
        if(mode==3)bad.rejectMap=true;
        if(mode==4)bad.map.shortMap=true;
        if(mode==5)bad.state=3;
        if(mode==6)bad.map.regs[0x2fc/4]=0xffffffff;
        assert(bus.bind(&bad,&owner)!=0&&bad.refs==1);
        assert(bad.opened==(mode==1));
        if(mode<=2)assert(bad.closes==0);
        else assert(bad.closes==1&&!bad.memory);
    }
    IOPCIDevice noMemory;allocationFail=true;
    {IntelLPSSSPIController bus;assert(bus.bind(&noMemory,&owner)==kIOReturnNoMemory&&noMemory.opens==0);}
    allocationFail=false;
    IOPCIDevice originalMemory;originalMemory.memory=true;
    {IntelLPSSSPIController bus;assert(bus.bind(&originalMemory,&owner)==0);}
    assert(originalMemory.memory&&allocs==frees);
    IOPCIDevice poweredOff;
    {IntelLPSSSPIController bus;assert(bus.bind(&poweredOff,&owner)==0);poweredOff.state=3;bus.shutdown();}
    assert(!poweredOff.opened&&poweredOff.refs==1&&allocs==frees);
}
''')
    for flags in [[],['-fsanitize=address,undefined','-fno-omit-frame-pointer']]:
        exe=tmp/'pci-test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,
                        '-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
                        str(tmp/'test.cpp'),str(ROOT/'CirrusAudioFixup/Transport/Intel/IntelLPSSSPIController.cpp'),
                        '-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('PASS production Intel SPI PCI owner: identity, exclusive ownership, D0, BAR, suspend, teardown and allocation failures')
