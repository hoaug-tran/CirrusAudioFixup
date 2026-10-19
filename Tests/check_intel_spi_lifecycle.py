from host_build import sanitizer_flags
"""Execute the production PCI SPI owner against IOKit lifecycle mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-spi-pci-') as directory:
    tmp=Path(directory)
    for name in ['IOKit/pci','IOKit/pwr_mgt','kern','mach']:
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
constexpr unsigned kIOPMPowerOn=2,kIOPMDeviceUsable=0x8000;
struct IOPMPowerState {unsigned version=0,capabilityFlags=0,outputPowerCharacter=0,inputPowerRequirement=0;};
inline unsigned pmClients=0,pmStops=0;
inline bool rejectPM=false,denyDomain=false,rejectVote=false;
class IOService {
public:
    IOService* powerProvider=nullptr;
    unsigned capability=0;
    void (*powerRequest)(IOService*,unsigned)=nullptr;
    bool init(){return true;}
    bool attach(IOService* p){powerProvider=p;++pmClients;return true;}
    void detach(IOService* p){assert(!lockStorage.held&&p==powerProvider);powerProvider=nullptr;}
    void release(){delete this;}
    void PMinit(){}
    void PMstop(){assert(!lockStorage.held);++pmStops;}
    IOReturn registerPowerDriver(IOService*,IOPMPowerState*,unsigned){return rejectPM?kIOReturnNotReady:0;}
    void joinPMtree(IOService* c){assert(c->powerProvider==this);}
    unsigned currentCapability(){return capability;}
    IOReturn changePowerStateTo(unsigned s){assert(!lockStorage.held);if(rejectVote)return kIOReturnIOError;capability=s&&!denyDomain?kIOPMDeviceUsable:0;if(powerProvider&&powerProvider->powerRequest)powerProvider->powerRequest(powerProvider,s);return 0;}
};
struct IOMemoryMap {
    uint32_t regs[0x300/4]{};unsigned releases=0;bool shortMap=false;
    IOMemoryMap(){regs[0x2fc/4]=0x20|(3<<9);regs[0x224/4]=3;}
    uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(regs);}
    size_t getLength(){return shortMap?0x100:sizeof(regs);}
    void release(){++releases;}
};
'''.replace('#include "IOLib.h"','#include "IOLib.h"\n#include <cstddef>'))
    for name in ['IOPM.h','IOPMpowerState.h']:
        (tmp/'IOKit/pwr_mgt'/name).write_text('#pragma once\n#include "IOKit/IOService.h"\n')
    (tmp/'IOKit/pci/IOPCIDevice.h').write_text(r'''
#pragma once
#include "IOKit/IOService.h"
constexpr uint8_t kIOPCIPowerManagementCapability=1,kIOPCIConfigCommand=4,kIOPCIConfigBaseAddress0=0x10;
struct IOPCIDevice : IOService {
    unsigned refs=1,opens=0,closes=0;
    bool opened=false,rejectOpen=false,rejectMap=false,rejectMemory=false,memory=false;
    uint16_t vendor=0x8086,device=0x7aaa,state=0;
    bool wakeAllowed=false;unsigned requests=0,writes=0;
    IOPCIDevice(){powerRequest=[](IOService* p,unsigned){auto* pci=static_cast<IOPCIDevice*>(p);++pci->requests;};}
    IOMemoryMap map;
    IOService* client=nullptr;
    uint16_t configRead16(uint8_t o){return o==0?vendor:o==2?device:o==4?(memory?2:0):o==0x44?state:0;}
    void configWrite16(uint8_t o,uint16_t v){assert(!lockStorage.held&&o==0x44&&!(v&0x8000));++writes;if(wakeAllowed)state=(state&0x8000)|v;}
    void retain(){++refs;}void release(){--refs;}
    bool isOpen(){return opened;}
    bool open(IOService* c){++opens;if(rejectOpen)return false;opened=true;client=c;return true;}
    void close(IOService* c){assert(!lockStorage.held);assert(opened&&client==c);opened=false;++closes;}
    uint32_t findPCICapability(uint8_t,uint8_t* out){*out=0x40;return 1;}
    bool setMemoryEnable(bool b){if(!rejectMemory)memory=b;return memory;}
    IOMemoryMap* mapDeviceMemoryWithRegister(uint8_t reg){assert(reg==0x10&&(state&3)==0);return rejectMap?nullptr:&map;}
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
#include <initializer_list>
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
        pci.state=0; pci.memory=false; pci.rejectMemory=true;
        assert(bus.resume()==kIOReturnNotReady);
        assert(bus.exchange(ep,tx,nullptr,sizeof(tx))==kIOReturnNotReady);
        pci.rejectMemory=false;assert(bus.resume()==0);
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
        const char* reasons[]={"INTEL_CONTROLLER_ID_UNSUPPORTED","INTEL_CONTROLLER_EXCLUSIVE_OPEN_FAILED","INTEL_CONTROLLER_EXCLUSIVE_OPEN_FAILED","INTEL_CONTROLLER_BAR_MAPPING_FAILED","INTEL_CONTROLLER_BAR_TOO_SMALL","INTEL_CONTROLLER_POWER_REQUEST_FAILED","INTEL_CONTROLLER_ENGINE_INITIALIZATION_FAILED"};
        assert(!strcmp(bus.bindFailureReason(),reasons[mode]));
        if(mode==6)assert(bus.initialCapabilities()==0xffffffffU);
        assert(bad.opened==(mode==1));
        if(mode<=2)assert(bad.closes==0);
        else assert(bad.closes==1&&!bad.memory);
    }
    for(uint16_t id : {uint16_t(0x9d29),uint16_t(0x1ac2),uint16_t(0xa32a),uint16_t(0x51aa)}) {
        IOPCIDevice supported;supported.device=id;IntelLPSSSPIController controller;
        assert(controller.bind(&supported,&owner)==0);controller.suspend();
        assert(controller.resume()==0);controller.shutdown();assert(supported.refs==1&&!supported.opened);
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
    // Target regression: HP arrives in D3 with memory decoding disabled.
    IOPCIDevice cold;cold.state=0x810b;cold.wakeAllowed=true;
    {IntelLPSSSPIController bus;assert(bus.bind(&cold,&owner)==0);
     assert(cold.requests==1&&cold.state==0x8108&&cold.memory&&cold.writes==1);
     bus.suspend();cold.state=0xb;assert(bus.resume()==0&&cold.state==8);
     bus.shutdown();}
    assert(cold.refs==1&&!cold.opened&&!cold.memory);
    IOPCIDevice resetRequired;resetRequired.state=3;resetRequired.wakeAllowed=true;
    {IntelLPSSSPIController bus;assert(bus.bind(&resetRequired,&owner)==kIOReturnNotReady);}
    assert(resetRequired.writes==0&&resetRequired.map.releases==0);
    for(unsigned failure=0;failure<3;++failure){
        IOPCIDevice denied;denied.state=3;denied.wakeAllowed=true;
        rejectPM=failure==0;denyDomain=failure==1;rejectVote=failure==2;
        auto start=fakeNs;
        {IntelLPSSSPIController bus;assert(bus.bind(&denied,&owner)!=0);}
        assert(denied.writes==0&&denied.map.releases==0&&!denied.memory&&!denied.opened&&denied.refs==1);
        if(failure==1)assert(fakeNs-start<=510000000);
        rejectPM=denyDomain=rejectVote=false;
    }
    assert(pmClients==pmStops);
}
''')
    for flags in sanitizer_flags():
        exe=tmp/'pci-test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,
                        '-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
                        str(tmp/'test.cpp'),str(ROOT/'CirrusAudioFixup/Transport/Intel/IntelLPSSSPIController.cpp'),
                        '-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('PASS production Intel SPI PCI owner: identity, exclusive ownership, D0, BAR, suspend, teardown and allocation failures')
