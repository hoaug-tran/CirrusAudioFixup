"""Minimal deterministic kernel resources for compiling the complete AMP driver.

No silicon helpers are mocked; only OS allocation, registry, time and resources.
"""
def install(root):
    files={
'IOKit/IOLib.h':r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include "Diagnostics/DiagnosticTypes.hpp"
#define OSSwapLittleToHostInt16(x) (x)
#define OSSwapLittleToHostInt32(x) (x)
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
using std::min;
constexpr int kIOReturnError=-1,kIOReturnNotReady=-2,kIOReturnBadArgument=-3,kIOReturnNotPermitted=-4,kIOReturnNotFound=-5,kIOReturnNoMemory=-6,kIOReturnUnsupported=-7,kIOReturnTimeout=-8;
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
inline void* IOMalloc(size_t n){return malloc(n);}inline void* IOMallocData(size_t n){return malloc(n);}
inline void IOFree(void* p,size_t){free(p);}inline void IOFreeData(void* p,size_t){free(p);}
inline uint64_t fixtureTimeMs=0;
inline void IOSleep(unsigned n){fixtureTimeMs+=n;}
inline uint64_t mach_absolute_time(){return fixtureTimeMs*1000000;}
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* n){*n=t;}
inline size_t strlcpy(char* d,const char* s,size_t n){size_t l=strlen(s);if(n){size_t c=std::min(l,n-1);memcpy(d,s,c);d[c]=0;}return l;}
inline size_t strlcat(char* d,const char* s,size_t n){size_t used=0;while(used<n&&d[used])++used;if(used==n)return n+strlen(s);return used+strlcpy(d+used,s,n-used);}
''',
'IOKit/IOService.h':r'''
#pragma once
#include "IOKit/IOLib.h"
#include <map>
#include <string>
#include <vector>
#include <cassert>
class OSObject {public:unsigned refs=1;virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs);if(!--refs)delete this;}};
#define OSDynamicCast(T,p) dynamic_cast<T*>(p)
#define OSSafeReleaseNULL(p) do{if(p){p->release();p=nullptr;}}while(0)
struct OSString:OSObject {std::string value;static OSString* withCString(const char* s){auto* o=new OSString;o->value=s;return o;}const char* getCStringNoCopy(){return value.c_str();}};
struct OSNumber:OSObject {uint32_t value=0;uint32_t unsigned32BitValue(){return value;}};
struct OSData:OSObject {std::vector<uint8_t> bytes;size_t getLength(){return bytes.size();}const void* getBytesNoCopy(){return bytes.data();}};
struct OSDictionary:OSObject {};
struct OSIterator:OSObject {std::vector<OSObject*> objects;size_t pos=0;OSObject* getNextObject(){return pos<objects.size()?objects[pos++]:nullptr;}};
inline unsigned registryLive=0;
struct IORegistryEntry:OSObject {
    std::map<std::string,OSObject*> properties;IORegistryEntry* parent=nullptr;
    virtual OSObject* getProperty(const char* key){return properties.count(key)?properties[key]:nullptr;}
    OSObject* copyProperty(const char* key){auto* p=getProperty(key);if(p)p->retain();return p;}
    IORegistryEntry* getParentEntry(const void*){return parent;}
    bool getPath(char*,int*,const void*){return false;}
    static IORegistryEntry* fromPath(const char*){return nullptr;}
};
inline const void* gIOServicePlane=nullptr;
inline std::vector<OSObject*> fixtureCodecs;
class IOService:public IORegistryEntry {
public:
    static OSDictionary* serviceMatching(const char*){return new OSDictionary;}
    static OSIterator* getMatchingServices(OSDictionary*){auto* i=new OSIterator;i->objects=fixtureCodecs;return i;}
};
''',
'IOKit/pci/IOPCIDevice.h':r'''
#pragma once
#include "IOKit/IOService.h"
constexpr unsigned kIOPCIConfigVendorID=0,kIOPCIConfigDeviceID=2,kIOPCIConfigSubSystemVendorID=4,kIOPCIConfigSubSystemID=6,kIOPCIConfigRevisionID=8;
class IOPCIDevice:public IOService {public:uint16_t configRead16(unsigned){return 0;}uint8_t configRead8(unsigned){return 0;}};
''',
'libkern/OSByteOrder.h':r'''
#pragma once
#define OSSwapLittleToHostInt16(x) (x)
#define OSSwapLittleToHostInt32(x) (x)
#define OSSwapHostToBigInt32(x) __builtin_bswap32(x)
#define OSSwapBigToHostInt32(x) __builtin_bswap32(x)
'''}
    for name in ['OSData','OSString','OSNumber']:
        files['libkern/c++/'+name+'.h']='#pragma once\n#include "IOKit/IOService.h"\n'
    for name,text in files.items():
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
