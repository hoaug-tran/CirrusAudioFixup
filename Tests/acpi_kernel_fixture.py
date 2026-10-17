"""Minimal ACPI registry/property/mapping resources for production parsers."""

def install(tmp):
    for name in ("IOKit", "IOKit/acpi", "IOKit/pci", "libkern/c++"):
        (tmp / name).mkdir(parents=True, exist_ok=True)
    for name in ("IOKit/IOLib.h", "IOKit/IORegistryEntry.h", "IOKit/acpi/IOACPIPlatformDevice.h",
                 "IOKit/pci/IOPCIDevice.h", "libkern/c++/OSArray.h", "libkern/c++/OSData.h",
                 "libkern/c++/OSNumber.h", "libkern/c++/OSString.h"):
        (tmp / name).write_text('#pragma once\n#include "Fixture.hpp"\n')
    (tmp / "Fixture.hpp").write_text(r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnUnsupported=-3,kIOReturnNoDevice=-4,kIOReturnIOError=-5,kIOReturnNotPermitted=-6,
kIOReturnNotFound=-7,kIOReturnExclusiveAccess=-8,kIOReturnNoResources=-9,kIOReturnTimeout=-10;
struct OSObject {unsigned refs=1;virtual ~OSObject()=default;virtual OSObject* metaCast(const char*){return nullptr;}
 void retain(){++refs;}void release(){assert(refs>1);--refs;}};
inline std::vector<std::unique_ptr<OSObject>> arena;
template<class T,class... A>T* make(A&&... a){auto p=std::make_unique<T>(std::forward<A>(a)...);auto r=p.get();arena.push_back(std::move(p));return r;}
#define OSDynamicCast(t,o) dynamic_cast<t*>(o)
#define OSSafeReleaseNULL(o) do{if(o){o->release();o=nullptr;}}while(0)
inline void OSSynchronizeIO(){}
inline void IOSleep(unsigned){}
struct OSString:OSObject {std::string s;explicit OSString(const char* v):s(v){}const char* getCStringNoCopy(){return s.c_str();}
 bool isEqualTo(const char* v){return s==v;}bool isEqualTo(OSString* v){return v&&s==v->s;}};
struct OSNumber:OSObject {uint64_t v;explicit OSNumber(uint64_t n):v(n){}uint64_t unsigned64BitValue(){return v;}uint32_t unsigned32BitValue(){return v;}};
struct OSData:OSObject {std::vector<uint8_t> v;explicit OSData(std::vector<uint8_t> b):v(std::move(b)){}size_t getLength(){return v.size();}void* getBytesNoCopy(){return v.data();}};
struct OSArray:OSObject {std::vector<OSObject*> v;explicit OSArray(std::vector<OSObject*> b):v(std::move(b)){}
 unsigned getCount(){return v.size();}OSObject* getObject(unsigned n){return n<v.size()?v[n]:nullptr;}};
struct IORegistryEntry:OSObject {std::string path;static const void* getPlane(const char*){return reinterpret_cast<void*>(1);}
 static IORegistryEntry* getRegistryRoot(){return nullptr;}IORegistryEntry* getParentEntry(const void*){return nullptr;}
 bool getPath(char* out,int* n,const void*){if(path.size()+1>unsigned(*n))return false;strcpy(out,path.c_str());return true;}};
class IOService:public IORegistryEntry {};
inline std::vector<IORegistryEntry*> devices;
constexpr unsigned kIORegistryIterateRecursively=1;
struct IORegistryIterator {unsigned i=0;static IORegistryIterator* iterateOver(IORegistryEntry*,const void*,unsigned){return new IORegistryIterator;}
 IORegistryEntry* getNextObject(){return i<devices.size()?devices[i++]:nullptr;}void release(){delete this;}};
struct IOMemoryMap:OSObject {uint32_t r[0x10000/4]{};size_t getLength(){return sizeof(r);}uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(r);}};
class IOACPIPlatformDevice:public IOService {public:std::map<std::string,OSObject*> values;IOMemoryMap* map=nullptr;bool opened=false;unsigned opens=0,closes=0;
 OSObject* metaCast(const char* n)override{return !strcmp(n,"IOACPIPlatformDevice")?this:nullptr;}
 IOReturn validateObject(const char* n){return values.count(n)?0:kIOReturnNotFound;}
 IOReturn evaluateObject(const char* n,OSObject** out){auto i=values.find(n);if(i==values.end())return kIOReturnNotFound;*out=i->second;(*out)->retain();return 0;}
 IOReturn evaluateInteger(const char* n,uint32_t* out){auto v=OSDynamicCast(OSNumber,values[n]);if(!v)return kIOReturnIOError;*out=v->v;return 0;}
 unsigned getDeviceStatus(){return 15;}bool isOpen(){return opened;}bool open(IOService*){opened=true;++opens;return true;}
 void close(IOService*){assert(opened);opened=false;++closes;}
 IOMemoryMap* mapDeviceMemoryWithIndex(unsigned n){if(n||!map)return nullptr;map->retain();return map;}
};
class IOPCIDevice:public IOService {};
''')
