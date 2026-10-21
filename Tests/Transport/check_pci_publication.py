"""Exercise the production PCI association method, including late publication."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
header = (root/'CirrusAudioFixup/Transport/SPI/NativeSPIHost.hpp').read_text()
start=header.index('    static IOPCIDevice* findPCI(')
end=header.index('    const char* controllerSource',start)
method=header[start:end]
fixture=r'''
#include <cassert>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
struct OSObject { int refs=1; virtual ~OSObject()=default; virtual OSObject* metaCast(const char*){return nullptr;} void retain(){++refs;} void release(){assert(refs>0);if(--refs==0)delete this;} };
#define OSDynamicCast(type,obj) dynamic_cast<type*>(obj)
#define OSSafeReleaseNULL(obj) do{if(obj){obj->release();obj=nullptr;}}while(0)
struct OSString:OSObject {std::string value;explicit OSString(const char* s):value(s){} static OSString* withCString(const char* s){return new OSString(s);}const char* getCStringNoCopy(){return value.c_str();}};
struct OSDictionary:OSObject {std::map<std::string,OSObject*> data;static OSDictionary* withCapacity(unsigned){return new OSDictionary;}void setObject(const char* key,OSObject* object){object->retain();auto& old=data[key];if(old)old->release();old=object;}OSObject* getObject(const char* key){return data[key];}~OSDictionary(){for(auto& v:data)if(v.second)v.second->release();}};
struct IORegistryEntry:OSObject {std::string path;static const void* getPlane(const char*){return reinterpret_cast<void*>(1);}bool getPath(char* out,int* size,const void*){if(path.size()+1>unsigned(*size))return false;strcpy(out,path.c_str());return true;}};
struct IOService;
static std::vector<IOService*> published,late;
static IOService* wakeService=nullptr;
static unsigned waits=0;
static uint64_t lastTimeout=0;
struct OSIterator {std::vector<IOService*> values;unsigned at=0;explicit OSIterator(const std::vector<IOService*>& v):values(v){}OSObject* getNextObject();void release(){delete this;}};
struct IOService:IORegistryEntry {
 std::map<std::string,OSObject*> properties;std::map<std::string,std::string> strings;std::map<std::string,uint64_t> numbers;
 OSObject* metaCast(const char* name)override{return !strcmp(name,"IOService")?this:nullptr;}
 OSObject* getProperty(const char* key){auto i=properties.find(key);return i==properties.end()?nullptr:i->second;}
 void setProperty(const char* key,const char* value){strings[key]=value;}
 void setProperty(const char* key,uint64_t value,unsigned){numbers[key]=value;}
 static OSDictionary* serviceMatching(const char*){return new OSDictionary;}
 static OSIterator* getMatchingServices(OSDictionary*){return new OSIterator(published);}
 static IOService* waitForMatchingService(OSDictionary* match,uint64_t timeout){
  ++waits;lastTimeout=timeout;
  auto* props=OSDynamicCast(OSDictionary,match->getObject("IOPropertyMatch"));assert(props);
  auto* path=OSDynamicCast(OSString,props->getObject("acpi-path"));assert(path);
  assert(path->value=="IOACPIPlane:/_SB/PC00/SPI0@1e0002");
  published=late;if(wakeService)wakeService->retain();return wakeService;
 }
};
OSObject* OSIterator::getNextObject(){return at<values.size()?values[at++]:nullptr;}
class IOACPIPlatformDevice:public IOService {public:OSObject* metaCast(const char* name)override{return !strcmp(name,"IOACPIPlatformDevice")?this:IOService::metaCast(name);}};
class IOPCIDevice:public IOService {public:OSObject* metaCast(const char* name)override{return !strcmp(name,"IOPCIDevice")?this:IOService::metaCast(name);}};
#define KERNEL 1
#include "Support/KernelDeviceCast.hpp"
#include "Platform/ACPI/ACPIPath.hpp"
using cirrus::transport::canonicalACPIPath;
static bool amdContract=false;
inline bool nativeSPIControllerHasACPIContract(IOACPIPlatformDevice*){return amdContract;}
struct Association {
'''
checks=r'''
};
int main(){
 IOACPIPlatformDevice target;target.path="IOACPIPlane:/_SB/PC00/SPI0@1e0002";
 IOService owner,foreign;OSString intel("INTEL"),amd("AMD"),path("IOACPIPlane:/_SB/PC00/SPI0@1e0002");
 owner.properties["Cirrus_CPU_Vendor"]=&intel;
 IOPCIDevice pci,duplicate;pci.properties["acpi-device"]=&target;pci.properties["acpi-path"]=&path;
 duplicate.properties=pci.properties;
 published={&pci};auto* result=Association::findPCI(&target,&owner);assert(result==&pci&&waits==0);result->release();
 // Canonical path association works even without the direct ACPI object property.
 pci.properties.erase("acpi-device");result=Association::findPCI(&target,&owner);assert(result==&pci&&waits==0);result->release();
 // The real HP ACPI path is published later; wait must target it and retain once.
 published.clear();late={&pci};wakeService=&pci;result=Association::findPCI(&target,&owner);
 assert(result==&pci&&waits==1&&lastTimeout==5000000000ULL);assert(owner.strings["Cirrus_SPI_PCI_Lookup_Status"]=="PUBLISHED_AFTER_WAIT");result->release();assert(pci.refs==1);
 // No published device: bounded timeout, no unrelated association.
 published.clear();late.clear();wakeService=nullptr;assert(!Association::findPCI(&target,&owner));assert(waits==2);
 // Existing duplicates are rejected without waiting.
 published={&pci,&duplicate};assert(!Association::findPCI(&target,&owner));assert(waits==2);
 // Duplicates arriving during the wait are rejected by the rescan too.
 published.clear();late={&pci,&duplicate};wakeService=&pci;assert(!Association::findPCI(&target,&owner));assert(waits==3&&pci.refs==1);
 // A wake with the wrong type is rejected and its owned reference returned.
 published.clear();late={&foreign};wakeService=&foreign;assert(!Association::findPCI(&target,&owner));assert(waits==4&&foreign.refs==1);
 // AMD ACPI ownership is not delayed by an Intel PCI wait.
 owner.properties["Cirrus_CPU_Vendor"]=&amd;amdContract=true;published.clear();assert(!Association::findPCI(&target,&owner));assert(waits==4);
 // CPU brand is informational: an unrecognized ACPI controller on an AMD
 // host still follows its PCI association, while an AMD contract skips it.
 amdContract=false;published.clear();late={&pci};wakeService=&pci;
 result=Association::findPCI(&target,&owner);assert(result==&pci&&waits==5);result->release();
 assert(target.refs==1&&pci.refs==1&&duplicate.refs==1);
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder)
    for name in ['IOKit/IORegistryEntry.h','IOKit/acpi/IOACPIPlatformDevice.h','IOKit/pci/IOPCIDevice.h']:
        f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('')
    cpp=p/'publication.cpp';cpp.write_text(fixture+method+checks)
    for flags in sanitizer_flags():
        binary=p/'publication'
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I',folder,'-I',str(root/'CirrusAudioFixup'),str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('PASS production PCI association: HP path, direct/path match, late publication, finite timeout, pre/post-wait ambiguity, wrong type, retain balance and AMD no-wait')
