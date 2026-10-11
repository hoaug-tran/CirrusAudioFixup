"""Run AMD ACPI/MMIO ownership code against failure and power-transition mocks."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-amd-acpi-') as directory:
    p=Path(directory)
    for d in ['IOKit/acpi','libkern/c++','kern','mach']: (p/d).mkdir(parents=True)
    (p/'IOKit/IOLib.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
using IOReturn=int;using UInt32=uint32_t;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnUnsupported=-3,kIOReturnBusy=-4,kIOReturnNoDevice=-5,kIOReturnTimeout=-6,
kIOReturnNoMemory=-7,kIOReturnExclusiveAccess=-8,kIOReturnNoResources=-9;
inline bool failAlloc=false;inline unsigned allocs=0,frees=0;
struct IOLock {bool held=false;};inline IOLock storage;
inline IOLock* IOLockAlloc(){if(failAlloc)return nullptr;++allocs;return &storage;}
inline void IOLockFree(IOLock* p){assert(!p->held);++frees;}
inline void IOLockLock(IOLock* p){assert(!p->held);p->held=true;}
inline void IOLockUnlock(IOLock* p){assert(p->held);p->held=false;}
inline uint64_t now=0;inline void IODelay(unsigned n){now+=uint64_t(n)*1000;}
inline void OSSynchronizeIO(){}
''')
    (p/'libkern/c++/OSString.h').write_text(r'''
#pragma once
#include <cstring>
struct OSObject {unsigned refs=1;virtual ~OSObject()=default;void release(){--refs;}};
struct OSString:OSObject {const char* value="AMDI0061";bool isEqualTo(const char* v){return !strcmp(value,v);}};
#define OSDynamicCast(type,obj) dynamic_cast<type*>(obj)
''')
    (p/'IOKit/acpi/IOACPIPlatformDevice.h').write_text(r'''
#pragma once
#include "IOKit/IOLib.h"
#include "libkern/c++/OSString.h"
struct IOService {};
struct IOMemoryMap {
 uint32_t bytes[0xc8/4]{};bool shortMap=false;unsigned releases=0;
 uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(bytes);}
 size_t getLength(){return shortMap?8:sizeof(bytes);}void release(){++releases;}
};
struct IOACPIPlatformDevice {
 OSString hid;IOMemoryMap map;unsigned refs=1,opens=0,closes=0;IOService* owner=nullptr;
 bool opened=false,failOpen=false,failMap=false,hasPSC=true,failPSC=false;UInt32 power=0;
 IOReturn evaluateObject(const char*,OSObject** out){++hid.refs;*out=&hid;return 0;}
 IOReturn validateObject(const char*){return hasPSC?0:-1;}
 IOReturn evaluateInteger(const char*,UInt32* out){*out=power;return failPSC?-1:0;}
 void retain(){++refs;}void release(){--refs;}bool isOpen(){return opened;}
 bool open(IOService* o){++opens;if(failOpen)return false;owner=o;opened=true;return true;}
 void close(IOService* o){assert(opened&&o==owner);++closes;opened=false;}
 IOMemoryMap* mapDeviceMemoryWithIndex(unsigned i){assert(i==0);return failMap?nullptr:&map;}
};
''')
    (p/'kern/clock.h').write_text('#pragma once\n#include "IOKit/IOLib.h"\ninline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* n){*n=t;}\n')
    (p/'mach/mach_time.h').write_text('#pragma once\n#include "IOKit/IOLib.h"\ninline uint64_t mach_absolute_time(){return ++now;}\n')
    (p/'test.cpp').write_text(r'''
#include "Transport/AMD/AMDSPIController.hpp"
using namespace cirrus::transport;using namespace cirrus::transport::amd;
int main(){
 IOService owner;SPIEndpoint ep{0,1000000,0};uint8_t tx[6]{},rx[4]{};
 for(unsigned revision=1;revision<=2;++revision){
  IOACPIPlatformDevice acpi;acpi.hid.value=revision==1?"AMDI0061":"AMDI0062";
  AMDSPIController bus;assert(bus.bind(&acpi,&owner)==0&&acpi.refs==2&&acpi.opened);
  assert(bus.bind(&acpi,&owner)==kIOReturnBusy);bus.suspend();
  assert(bus.command(ep,tx,6,rx,4)==kIOReturnNotReady);
  acpi.power=3;assert(bus.resume()==kIOReturnNotReady);
  acpi.power=0;assert(bus.resume()==0);
  // Native MMIO model has no command sequencer; only test no-access guards.
  bus.suspend();acpi.power=3;bus.shutdown();assert(acpi.refs==1&&!acpi.opened&&acpi.map.releases==1);
  acpi.power=0;assert(bus.bind(&acpi,&owner)==0);bus.shutdown();
  assert(acpi.hid.refs==1&&acpi.refs==1&&acpi.closes==2);
 }
 for(unsigned failure=0;failure<7;++failure){
  IOACPIPlatformDevice acpi;AMDSPIController bus;
  if(failure==0)acpi.hid.value="AMDI9999";
  if(failure==1)acpi.opened=true;
  if(failure==2)acpi.failOpen=true;
  if(failure==3)acpi.failMap=true;
  if(failure==4)acpi.map.shortMap=true;
  if(failure==5)acpi.power=3;
  if(failure==6)acpi.failPSC=true;
  assert(bus.bind(&acpi,&owner)!=0&&acpi.refs==1&&acpi.hid.refs==1);
  assert(acpi.opened==(failure==1));
 }
 IOACPIPlatformDevice acpi;AMDSPIController bus;failAlloc=true;
 assert(bus.bind(&acpi,&owner)==kIOReturnNoMemory&&acpi.opens==0);failAlloc=false;
 acpi.hasPSC=false;assert(bus.bind(&acpi,&owner)==0);bus.shutdown();assert(allocs==frees);
}
''')
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),str(ROOT/'CirrusAudioFixup/Transport/AMD/AMDSPIController.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
print('PASS AMD ACPI owner: HID, exclusive access, mapping, D0, allocation cleanup and repeat bind')
