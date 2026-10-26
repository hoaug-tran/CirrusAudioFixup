#pragma once
#include "Fixture.hpp"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>
#include "Support/BuildConfig.hpp"

#define OSSwapLittleToHostInt32(x) (x)
inline size_t strlcpy(char* out,const char* in,size_t capacity){size_t length=strlen(in);if(capacity){size_t n=std::min(length,capacity-1);memcpy(out,in,n);out[n]=0;}return length;}
using mach_vm_address_t=uintptr_t;
constexpr bool kOSBooleanTrue=true,kOSBooleanFalse=false;
inline bool gCirrusDebug=false;
inline uint64_t nowNs=0,pciAtNs=0,powerAtNs=0,callbackAtNs=0,bootReadyAtNs=0,resetBootDelayNs=180000000;
inline bool callbackDone=false,earlyDone=false,missingSymbol=false,malformedDSD=false,stopped=false;
inline unsigned frames=0,writes=0,csBinds=0,liveControllers=0,allocations=0,evaluations=0,idReads=0,earlyIdReads=0;
inline std::vector<IOService*> published;
inline IOPCIDevice* publishedPCI=nullptr;
inline std::function<void()> callbackAction,earlyAction;
void replayAdvance(uint64_t);
inline uint64_t mach_absolute_time(){return nowNs;}
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* out){*out=t;}
inline void IOSleep(unsigned ms){replayAdvance(nowNs+uint64_t(ms)*1000000);}
void* IOMalloc(size_t);
void IOFree(void*,size_t);
#define IOLog(...) ((void)0)
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
inline bool PE_parse_boot_argn(const char*,void*,size_t){return false;}

class KernelPatcher {
public:
 enum class Error {NoError,AlreadyDone,KernRunningInitFailure};
 Error error=Error::NoError;
 bool poisoned=false;
 uintptr_t symbol(const char*);
 template<class T>T solveSymbol(size_t index,const char* name,uintptr_t start,size_t size){
  assert(index==1);assert(start==0xFFFFFF8008C01000ULL&&size==491520);
  return poisoned?0:static_cast<T>(symbol(name));
 }
 template<class T>size_t loadKinfo(T* info){info->loadIndex=1;return 1;}
 void updateRunningInfo(size_t index,uintptr_t address=0,size_t=0){
  assert(index==1);if(!address){poisoned=true;error=Error::KernRunningInitFailure;}
 }
 Error getError(){return error;}void clearError(){error=Error::NoError;}
};

inline OSDictionary* IOService::serviceMatching(const char* name){assert(!strcmp(name,"IOPCIDevice"));return OSDictionary::withCapacity(1);}
inline OSIterator* IOService::getMatchingServices(OSDictionary*){return new OSIterator{published,0};}
inline IOService* IOService::waitForMatchingService(OSDictionary* match,uint64_t timeout){
 auto* properties=OSDynamicCast(OSDictionary,match->getObject("IOPropertyMatch"));assert(properties);
 auto* path=OSDynamicCast(OSString,properties->getObject("acpi-path"));assert(path&&publishedPCI);
 assert(path->s=="IOACPIPlane:/_SB/PC00@0/SPI0@1e0002");
 const auto deadline=nowNs+timeout;
 if(pciAtNs>deadline){replayAdvance(deadline);return nullptr;}
 replayAdvance(std::max(nowNs,pciAtNs));published={publishedPCI};publishedPCI->retain();return publishedPCI;
}
