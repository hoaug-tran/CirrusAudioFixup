"""Production native SPI discovery, namespace lookup, ownership and PM failures."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text()
def owner_function(name):
 start=SOURCE.index(('size_t' if name=='detectNativeSPIAmplifiers' else 'bool' if name=='setupNativeSPI' else 'void')+' CirrusAudioFixup::'+name+'(')
 return SOURCE[start:SOURCE.index('\n}',start)+2]

with tempfile.TemporaryDirectory(prefix='cirrus-native-spi-') as directory:
 p=Path(directory)
 for name in ('IOKit','Transport/Intel','Transport/AMD'):(p/name).mkdir(parents=True,exist_ok=True)
 (p/'IOKit/IOLib.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnNotFound=-3,kIOReturnUnsupported=-4,kIOReturnNoMemory=-5,kIOReturnNotPermitted=-6,kIOReturnTimeout=-7;
inline void IOSleep(unsigned){}
''')
 (p/'IOKit/IORegistryEntry.h').write_text(r'''
#pragma once
#include "IOKit/IOLib.h"
#include <vector>
#include <string>
#include <cassert>
struct OSObject {unsigned refs=1;virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs);--refs;}};
#define OSDynamicCast(type,obj) dynamic_cast<type*>(obj)
#define OSSafeReleaseNULL(obj) do{if(obj){obj->release();obj=nullptr;}}while(0)
struct OSString:OSObject {std::string value;const char* getCStringNoCopy(){return value.c_str();}};
struct OSData:OSObject {std::vector<uint8_t> bytes;const void* getBytesNoCopy(){return bytes.data();}size_t getLength(){return bytes.size();}};
struct IORegistryPlane {};
inline IORegistryPlane plane;
struct IORegistryEntry;
inline IORegistryEntry* root=nullptr;
inline std::vector<IORegistryEntry*> nodes,pciNodes;
inline bool iteratorFailure=false;
inline unsigned iteratorLive=0;
struct IORegistryEntry:OSObject {
 std::string path;IORegistryEntry* parent=nullptr;
 static IORegistryEntry* getRegistryRoot(){return root;}
 static IORegistryPlane* getPlane(const char*){return &plane;}
 bool getPath(char* out,int* n,const IORegistryPlane*){if(path.size()+1>size_t(*n))return false;memcpy(out,path.c_str(),path.size()+1);return true;}
 IORegistryEntry* getParentEntry(const IORegistryPlane*){return parent;}
};
struct OSIterator {std::vector<IORegistryEntry*> list;size_t pos=0;OSIterator(std::vector<IORegistryEntry*> v):list(v){++iteratorLive;}virtual ~OSIterator(){--iteratorLive;}OSObject* getNextObject(){return pos<list.size()?list[pos++]:nullptr;}void release(){delete this;}};
constexpr int kIORegistryIterateRecursively=1;
struct IORegistryIterator:OSIterator {using OSIterator::OSIterator;static IORegistryIterator* iterateOver(IORegistryEntry*,const IORegistryPlane*,int){return iteratorFailure?nullptr:new IORegistryIterator(nodes);}};
struct IOService:IORegistryEntry {static void* serviceMatching(const char*){return nullptr;}static OSIterator* getMatchingServices(void*){return iteratorFailure?nullptr:new OSIterator(pciNodes);}};
struct IOACPIPlatformDevice:IOService {
 OSData crs;unsigned status=3,power=0;bool rejectCRS=false;bool amd=false,hasPSC=true;
 int evaluateInteger(const char*,uint32_t* out){*out=power;return hasPSC?0:kIOReturnNotFound;}
 unsigned getDeviceStatus(){return status;}
 int evaluateObject(const char*,OSObject** out){if(rejectCRS)return kIOReturnNotFound;crs.retain();*out=&crs;return 0;}
};
struct IOPCIDevice:IOService {IOACPIPlatformDevice* acpi=nullptr;bool supported=true;OSString acpiPath;OSObject* getProperty(const char* key){return !strcmp(key,"acpi-device")?static_cast<OSObject*>(acpi):!strcmp(key,"acpi-path")&&!acpiPath.value.empty()?&acpiPath:nullptr;}};
inline unsigned liveControllers=0,suspends=0,resumes=0;
inline bool resumeFailure=false;
inline uint32_t probeIDs[4]{};
''')
 for name,kind in (('Intel/IntelLPSSSPIController.hpp','intel'),('AMD/AMDACPIController.hpp','amd')):
  cls='IntelLPSSSPIController' if kind=='intel' else 'AMDACPIController'
  typ='IOPCIDevice' if kind=='intel' else 'IOACPIPlatformDevice'
  valid='p->supported' if kind=='intel' else 'p->amd'
  (p/'Transport'/name).write_text(f'''#pragma once
#include "IOKit/IORegistryEntry.h"
#include "Transport/SPIController.hpp"
namespace cirrus::transport::{kind} {{
class {cls}:public SPIController {{
 bool active=false;
public:
 {cls}(){{++liveControllers;}}~{cls}(){{--liveControllers;}}
 IOReturn bind({typ}* p,IOService*){{active={valid};return active?0:kIOReturnUnsupported;}}
 size_t maxTransferBytes() const override {{return 262;}}
 IOReturn exchange(const SPIEndpoint&,const uint8_t*,uint8_t*,size_t) override {{return kIOReturnUnsupported;}}
 IOReturn command(const SPIEndpoint& ep,const uint8_t*,size_t,uint8_t* rx,size_t n) override {{if(!active)return kIOReturnNotReady;for(size_t i=0;i<n;++i)rx[i]=n==4?uint8_t(probeIDs[ep.chipSelect]>>(24-8*i)):0;return 0;}}
 void suspend(){{++suspends;active=false;}}
 IOReturn resume(){{++resumes;active=!resumeFailure;return active?0:kIOReturnNotReady;}}
}};
}}
''')
 (p/'test.cpp').write_text(r'''
#include <cstring>
#include <cassert>
#include "Transport/NativeSPIHost.hpp"
#include "Core/DeviceRegistry.hpp"
#include "Transport/CS35L41SPITransport.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include <map>
#include <cstdio>
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
constexpr bool kOSBooleanFalse=false;
struct Amp {
 const char* name=nullptr;uint8_t address=0;cirrus::core::AudioChannel channel{};
 cirrus::core::RegisterIO* registerIO=nullptr;cirrus::core::BusEndpoint endpoint{};uint8_t endpointIndex=0;
 cirrus::core::CodecModel model{};bool present=false;uint32_t deviceId=0;
};
class CirrusAudioFixup:public IOService {
public:
 static constexpr size_t kMaxAmps=4;size_t mAmpCount=0;bool mProbingAmplifiers=false;Amp mAmps[4]{};
 cirrus::transport::NativeSPIHost mNativeSPI;
 std::map<std::string,uint64_t> numbers;std::map<std::string,std::string> strings;
 void setProperty(const char* k,uint64_t v,unsigned){numbers[k]=v;}
 void setProperty(const char* k,const char* v){strings[k]=v;}
 bool mPlatformAuthorized=true;std::map<std::string,bool> flags;
 bool bootArgEnabled(const char* k){return flags[k];}
 void setProperty(const char* k,bool value){numbers[k]=value;}
 bool setupNativeSPI(IOService*);
 size_t detectNativeSPIAmplifiers();void teardownNativeSPI();
};
'''+owner_function('setupNativeSPI')+owner_function('detectNativeSPIAmplifiers')+owner_function('teardownNativeSPI')+r'''
using namespace cirrus::transport;
std::vector<uint8_t> descriptor(const char* source,unsigned cs){
 size_t length=21+strlen(source)+1;std::vector<uint8_t> d(length);
 d[0]=0x8e;d[1]=length-3;d[3]=1;d[5]=2;d[6]=2;d[9]=1;d[10]=9;
 unsigned speed=1000000;for(int i=0;i<4;++i)d[12+i]=speed>>(i*8);
 d[16]=8;d[19]=cs;memcpy(d.data()+21,source,strlen(source)+1);return d;
}
void resources(IOACPIPlatformDevice& amp,std::initializer_list<std::pair<const char*,unsigned>> endpoints){
 amp.crs.bytes.clear();for(auto e:endpoints){auto d=descriptor(e.first,e.second);amp.crs.bytes.insert(amp.crs.bytes.end(),d.begin(),d.end());}amp.crs.bytes.push_back(0x79);amp.crs.bytes.push_back(0);
}
int main(){
 char path[256]{};
 assert(canonicalACPIPath("IOACPIPlane:/_SB/PCI0@0/SPI1@100000",path,sizeof(path))&&!strcmp(path,"\\_SB.PCI0.SPI1"));
 assert(canonicalACPIPath("IOACPIPlane:/\\_SB/PCI0@0/SPI1@100000",path,sizeof(path))&&!strcmp(path,"\\_SB.PCI0.SPI1"));
 assert(resolveACPIPath("\\_SB.PCI0.AMP0","^SPI1",path,sizeof(path))&&!strcmp(path,"\\_SB.PCI0.SPI1"));
 assert(!resolveACPIPath("\\","^SPI1",path,sizeof(path)));
 assert(!canonicalACPIPath("SPI1",path,sizeof(path)));
 assert(!resolveACPIPath("\\_SB","SPI1@0",path,sizeof(path)));
 assert(!canonicalACPIPath("\\_SB..SPI1",path,sizeof(path)));
 assert(!canonicalACPIPath("\\_SB.1234",path,sizeof(path)));
 assert(!canonicalACPIPath("\\_SB.",path,sizeof(path)));
 assert(!canonicalACPIPath("\\LONGNAME",path,sizeof(path)));
 IORegistryEntry namespaceRoot;namespaceRoot.path="IOACPIPlane:/";root=&namespaceRoot;
 IOACPIPlatformDevice sb,amp,a,b;
 sb.path="IOACPIPlane:/_SB";sb.parent=&namespaceRoot;
 amp.path="IOACPIPlane:/_SB/AMP0";amp.parent=&sb;
 a.path="IOACPIPlane:/_SB/SPI0";a.parent=&sb;
 b.path="IOACPIPlane:/_SB/SPI1";b.parent=&sb;b.amd=true;
 IOService owner;IOPCIDevice pci;pci.acpi=&a;pciNodes={&pci};nodes={&sb,&amp,&a,&b};
 cirrus::core::RegisterMapConfig format;format.paddingBytes=2;format.readFlag=0x80;
 NativeSPIHost host;
 resources(amp,{{"SPI0",1},{"\\_SB.SPI0",0},{"SPI1",2}});
 assert(host.bind(&amp,&owner,format)==0&&host.count()==3&&liveControllers==2);
 assert(host.bind(&amp,&owner,format)==kIOReturnBadArgument);
 assert(host.endpoint(0).spiChipSelect==1&&!strcmp(host.endpoint(0).controllerPath,"\\_SB.SPI0"));
 assert(a.refs==2&&b.refs==2&&amp.crs.refs==1&&!iteratorLive);
 uint32_t value=123;assert(host.registers(0)->read(0,&value)&&value==0);
 assert(!host.registers(0)->write(0,123)&&host.registers(0)->lastError()==kIOReturnNotPermitted);
 host.suspend();value=123;assert(!host.registers(0)->read(0,&value)&&value==123);
 resumeFailure=true;assert(host.resume()==kIOReturnNotReady);assert(!host.registers(1)->read(0,&value));
 resumeFailure=false;b.power=3;assert(host.resume()==kIOReturnNotReady);b.power=0;assert(host.resume()==0&&host.registers(2)->read(0,&value));
 host.shutdown();host.shutdown();assert(!liveControllers&&a.refs==1&&b.refs==1&&!iteratorLive);
 resources(amp,{{"SPI0",0}});amp.crs.bytes[4]=1;assert(host.bind(&amp,&owner,format)==kIOReturnUnsupported&&!liveControllers);
 pci.acpi=nullptr;pci.acpiPath.value=a.path;resources(amp,{{"SPI0",0}});
 assert(host.bind(&amp,&owner,format)==0&&host.count()==1);host.shutdown();
 pci.acpi=&a;pci.acpiPath.value.clear();
 resources(amp,{{"MISSING",0}});assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!host.bound()&&!liveControllers);
 resources(amp,{{"SPI0",0},{"SPI1",1}});b.hasPSC=false;
 assert(host.bind(&amp,&owner,format)==kIOReturnNotReady&&!liveControllers&&a.refs==1&&b.refs==1);b.hasPSC=true;
 b.amd=false;
 assert(host.bind(&amp,&owner,format)==kIOReturnUnsupported&&!host.bound()&&!liveControllers&&a.refs==1&&b.refs==1);b.amd=true;
 resources(amp,{{"SPI0",0},{"SPI0",0}});assert(host.bind(&amp,&owner,format)==kIOReturnBadArgument&&!liveControllers);
 resources(amp,{{"SPI0",0},{"\\_SB.SPI0",0}});assert(host.bind(&amp,&owner,format)==kIOReturnBadArgument&&!liveControllers);
 resources(amp,{{"SPI0",0}});amp.status=0;assert(host.bind(&amp,&owner,format)==kIOReturnNotReady);amp.status=3;
 amp.rejectCRS=true;assert(host.bind(&amp,&owner,format)==kIOReturnNotFound);amp.rejectCRS=false;
 iteratorFailure=true;assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!liveControllers);iteratorFailure=false;
 IOACPIPlatformDevice duplicate;duplicate.path=a.path;nodes.push_back(&duplicate);
 assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!liveControllers&&a.refs==1);nodes.pop_back();
 format.addressBytes=1;format.valueBytes=1;format.stride=1;format.paddingBytes=0;format.readFlag=0;format.maxRegister=255;
 assert(host.bind(&amp,&owner,format)==0);uint8_t bytes[3]{};assert(host.registers(0)->bulkRead(1,bytes,3));
 host.shutdown();assert(!liveControllers&&!iteratorLive&&amp.crs.refs==1);
 CirrusAudioFixup gated;IOService i2c;
 assert(gated.setupNativeSPI(&i2c)&&gated.strings["Cirrus_Bus"]=="I2C");
 assert(!gated.setupNativeSPI(&amp)&&!gated.mNativeSPI.bound());
 gated.flags["-cirrusspi"]=true;assert(!gated.setupNativeSPI(&amp));
 gated.flags["-cirrusro"]=true;assert(gated.setupNativeSPI(&amp)&&!gated.mPlatformAuthorized);
 assert(gated.strings["Cirrus_SPI_Status"]=="BOUND_READ_ONLY");
 assert(!gated.mNativeSPI.registers(0)->write(0,1));gated.teardownNativeSPI();assert(!liveControllers);
 // Actual shipping discovery must preserve resource identity after a failed probe.
 CirrusAudioFixup service;format={};format.paddingBytes=2;format.readFlag=0x80;
 resources(amp,{{"SPI0",1},{"SPI0",0},{"SPI1",2}});
 probeIDs[0]=probeIDs[2]=0x35a40;probeIDs[1]=0xdead;
 assert(service.mNativeSPI.bind(&amp,&service,format)==0);
 assert(service.detectNativeSPIAmplifiers()==2);
 assert(service.mAmps[0].endpointIndex==1&&service.mAmps[0].endpoint.spiChipSelect==0);
 assert(!strcmp(service.mAmps[0].name,"spi1")&&service.mAmps[0].address==0xff);
 assert(service.mAmps[0].channel==cirrus::core::AudioChannel::MaxChannels);
 assert(service.mAmps[1].endpointIndex==2&&!strcmp(service.mAmps[1].name,"spi2"));
 assert(service.numbers["Cirrus_SPI_0_DeviceID"]==0xdead&&service.numbers["Cirrus_SPI_Resource_Count"]==3);
 service.teardownNativeSPI();assert(!service.mAmps[0].registerIO&&!liveControllers&&!iteratorLive);

}
''')
 for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('PASS native SPI host: ACPI lookup, multi-controller ownership, read-only ports, partial rollback and PM faults; hardware untested')
