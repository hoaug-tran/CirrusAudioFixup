"""Production native SPI discovery, namespace lookup, ownership and PM failures."""
from pathlib import Path
import subprocess,tempfile
from service_runtime_fixture import PLATFORM_ADAPTER
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text()
def owner_function(name):
 if name=='createI2CController':
  start=SOURCE.index('cirrus::transport::I2CController* CirrusAudioFixup::createI2CController(')
  return SOURCE[start:SOURCE.index('\n}',start)+2]
 start=SOURCE.index(('size_t' if name.startswith('detect') else 'bool' if name in ('setupCommunication','preflightI2CIdentity','bindAmplifierDrivers') else 'void')+' CirrusAudioFixup::'+name+'(')
 return SOURCE[start:SOURCE.index('\n}',start)+2]

HEADER=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text()
def power_wrapper(name):
 import re
 start=re.search(r'(?:void|bool) '+name+r'\(',HEADER).start();opening=HEADER.index('{',start);depth=0
 for token in re.finditer(r'"(?:\\.|[^"\\])*"|//[^\n]*|[{}]',HEADER[opening:]):
  if token.group()=='{':depth+=1
  elif token.group()=='}':
   depth-=1
   if not depth:return HEADER[start:opening+token.end()]
 raise ValueError(name)
with tempfile.TemporaryDirectory(prefix='cirrus-native-spi-') as directory:
 p=Path(directory)
 for name in ('IOKit','Transport/Intel','Transport/AMD'):(p/name).mkdir(parents=True,exist_ok=True)
 (p/'IOKit/IOLib.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include "Diagnostics/DiagnosticTypes.hpp"
constexpr int kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnNotFound=-3,kIOReturnUnsupported=-4,kIOReturnNoMemory=-5,kIOReturnNotPermitted=-6,kIOReturnTimeout=-7;
inline void IOSleep(unsigned){}
''')
 (p/'IOKit/IORegistryEntry.h').write_text(r'''
#pragma once
#include "IOKit/IOLib.h"
#include <vector>
#include <string>
#include <cassert>
class OSObject {public:unsigned refs=1;virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs);--refs;}};
#define OSDynamicCast(type,obj) dynamic_cast<type*>(obj)
#define OSSafeReleaseNULL(obj) do{if(obj){obj->release();obj=nullptr;}}while(0)
struct OSString:OSObject {std::string value;const char* getCStringNoCopy(){return value.c_str();}};
struct OSData:OSObject {std::vector<uint8_t> bytes;const void* getBytesNoCopy(){return bytes.data();}size_t getLength(){return bytes.size();}};
struct IORegistryPlane {};
inline IORegistryPlane plane;
inline IORegistryPlane* gIOServicePlane=&plane;
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
class IOService:public IORegistryEntry {public:bool addressedABI=true;void* metaCast(const char*){return addressedABI?this:nullptr;}static void* serviceMatching(const char*){return nullptr;}static OSIterator* getMatchingServices(void*){return iteratorFailure?nullptr:new OSIterator(pciNodes);}};
class IOACPIPlatformDevice:public IOService {public:
 OSData crs;OSString hid;unsigned status=3,power=0;bool rejectCRS=false;bool amd=false,hasPSC=true;
 int evaluateInteger(const char*,uint32_t* out){*out=power;return hasPSC?0:kIOReturnNotFound;}
 unsigned getDeviceStatus(){return status;}
 int evaluateObject(const char* method,OSObject** out){if(!strcmp(method,"_HID")){if(hid.value.empty())return kIOReturnNotFound;hid.retain();*out=&hid;return 0;}if(rejectCRS)return kIOReturnNotFound;crs.retain();*out=&crs;return 0;}
};
class IOPCIDevice:public IOService {public:uint16_t configRead16(unsigned offset){return offset?0x7aaa:0x8086;}IOACPIPlatformDevice* acpi=nullptr;bool supported=true;OSString acpiPath;OSObject* getProperty(const char* key){return !strcmp(key,"acpi-device")?static_cast<OSObject*>(acpi):!strcmp(key,"acpi-path")&&!acpiPath.value.empty()?&acpiPath:nullptr;}};
inline unsigned liveControllers=0,suspends=0,resumes=0;
inline bool resumeFailure=false;
inline uint32_t probeIDs[4]{};
inline unsigned writeTransfers=0;inline std::vector<std::vector<uint8_t>> writes;
''')
 for name,kind in (('Intel/IntelLPSSSPIController.hpp','intel'),('AMD/AMDSPIController.hpp','amd')):
  cls='IntelLPSSSPIController' if kind=='intel' else 'AMDSPIController'
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
 IOReturn command(const SPIEndpoint& ep,const uint8_t* tx,size_t tn,uint8_t* rx,size_t n) override {{if(!active)return kIOReturnNotReady;if(!n){{++writeTransfers;writes.emplace_back(tx,tx+tn);}}for(size_t i=0;i<n;++i)rx[i]=n==4?uint8_t(probeIDs[ep.chipSelect]>>(24-8*i)):0;return 0;}}
 void suspend(){{++suspends;active=false;}}
 IOReturn resume(){{++resumes;active=!resumeFailure;return active?0:kIOReturnNotReady;}}
}};
}}
''')
 (p/'Transport/VoodooI2CController.hpp').write_text(r'''
#pragma once
#include "Transport/I2CController.hpp"
#include "Transport/I2CRegisterBus.hpp"
#include "IOKit/IORegistryEntry.h"
inline std::vector<std::vector<uint8_t>> i2cWrites,i2cReads;
inline uint32_t i2cProbeID=0x35a40;inline IOReturn i2cResult=0;
namespace cirrus::transport {
class VoodooI2CController:public I2CController {
public:
 explicit VoodooI2CController(IOService*){}
 size_t maxWriteBytes()const override{return 262;}
 size_t maxReadBytes()const override{return 256;}
 IOReturn transfer(const I2CEndpoint&,const uint8_t* tx,size_t tn,uint8_t* rx,size_t n)override {
  if(!n)i2cWrites.emplace_back(tx,tx+tn);else i2cReads.emplace_back(tx,tx+tn);
  if(i2cResult)return i2cResult;
  for(size_t i=0;i<n;++i)rx[i]=n==4?uint8_t(i2cProbeID>>(24-8*i)):0;
  return 0;
 }
};
}
''')
 (p/'test.cpp').write_text(r'''
#include <cstring>
#include <cassert>
#include "Transport/NativeSPIHost.hpp"
#include "Platform/SPIAmplifierManager.hpp"
#include "Platform/AmplifierHost.hpp"
#include "Diagnostics/AttachmentState.hpp"
using namespace cirrus::diagnostics;
#include "Platform/PlatformProfile.hpp"
#include "Core/AmplifierDescriptors.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp"
#include "Transport/VoodooI2CController.hpp"
#include <map>
#include <cstdio>
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) do{if(false)fprintf(stderr,__VA_ARGS__);}while(0)
constexpr bool kOSBooleanFalse=false,kOSBooleanTrue=true;
inline bool PE_parse_boot_argn(const char*,void*,int){return false;}
struct Amp {
 bool present=false,initialized=false,playbackFaulted=false;uint32_t deviceId=0,revisionId=0;cirrus::diagnostics::DiagnosticState diagnostic{};
 char endpointLabel[16]{};const char* name=nullptr;uint8_t address=0;cirrus::core::AudioChannel channel{};
 cirrus::core::RegisterIO* registerIO=nullptr;cirrus::core::BusEndpoint endpoint{};uint8_t endpointIndex=0;
 cirrus::core::AmplifierModel model{};
};
using AmplifierState=Amp;
class CirrusAudioFixup:public IOService {
public:
 static constexpr size_t kMaxAmplifiers=4;size_t mAmplifierCount=0;bool mProbingAmplifiers=false;Amp mAmplifiers[4]{};
 cirrus::transport::NativeSPIHost mNativeSPI;
 std::map<std::string,uint64_t> numbers;std::map<std::string,std::string> strings;
 void setProperty(const char* k,uint64_t v,unsigned){numbers[k]=v;}
 void setProperty(const char* k,const char* v){strings[k]=v;}
 bool mPlatformAuthorized=true;std::map<std::string,bool> flags;
 bool bootArgEnabled(const char* k){return flags[k];}
 void setProperty(const char* k,bool value){numbers[k]=value;}
 const cirrus::platform::PlatformProfile* mPlatformProfile=cirrus::platform::findPlatformProfile("CSC3551");
 uint32_t mPlatformSubsystemId=0;
 cirrus::platform::AmplifierDriverRegistry mDriverRegistry;
 IOService* mProvider=nullptr;char mRuntimeControllerPath[256]{};
 cirrus::platform::AmplifierManager mAmplifierManager{false};
 cirrus::core::RegisterIO* mRuntimePorts[4]{};
 cirrus::transport::I2CController* mI2CController=nullptr;
 cirrus::transport::I2CRegisterBus* mI2CBuses[4]{};
 cirrus::core::RegisterMap* mI2CMaps[4]{};
'''+PLATFORM_ADAPTER+power_wrapper('suspendCommunication')+power_wrapper('resumeCommunication')+r'''
 AmplifierHostAdapter mHostAdapters[4]{};
 cirrus::platform::AmplifierBinding mRuntimeBindings[4]{};
 cirrus::firmware::FirmwareBundle mRuntimeFirmware[4]{};
 cirrus::platform::BoardConfiguration mRuntimeBoard{};
 template<class... Args> void recordDiagnosticFailure(Amp&,cirrus::diagnostics::DiagnosticFailure,Args...){}
 // These tests use synthetic registered drivers to audit resource ownership.
 // No chip state or silicon callbacks exist in this generic service fixture.
 bool stopAfterDebugStage(const char*){assert(false);return false;}
 bool bootArgStrEquals(const char*,const char*){assert(false);return false;}
 bool mDebugPhaseHalted=false;
 void removeProperty(const char*){}
 void publishDriverVerdict(){}
 IOService* audioController(){return nullptr;}
 cirrus::platform::hda::HDAStreamState mHdaState{};
 struct Events{bool unchanged(uint32_t){return true;}};Events* mAudioEvents=nullptr;
 bool synchronizeHdaStream(){return false;}
 bool mCapturingFailureSnapshot=false;
 cirrus::diagnostics::AttachmentState mAttachment{};
 unsigned attachmentTraces=0;
 void recordTrace(cirrus::diagnostics::TraceSource,uint8_t,bool,bool,uint32_t,uint32_t,IOReturn){++attachmentTraces;}
 void recordAttachmentFailure(const char*,const char*,IOReturn,uint32_t=0,const char* kind="ENDPOINT");
 void publishAttachmentStatus();
 unsigned hostReports=0;
 void setDiagnosticStage(Amp&,cirrus::diagnostics::DriverStage){++hostReports;}
 void markDiagnosticSuccess(Amp&,cirrus::diagnostics::DriverStage){++hostReports;}
 cirrus::transport::I2CController* createI2CController();
 bool publishRuntimeStatus(){return false;}
 bool bindAmplifierDrivers();void releaseAmplifierDrivers();
 size_t expectedAmplifierCount()const{return mPlatformProfile->boardConfiguration?mPlatformProfile->boardConfiguration->count:mPlatformProfile->endpointCount;}
 size_t detectConfiguredI2CAmplifiers();
 ~CirrusAudioFixup(){teardownCommunication();}
 bool setupCommunication(IOService*);
 bool preflightI2CIdentity(const cirrus::platform::AmplifierDriverRegistration*);
 size_t detectNativeSPIAmplifiers();void teardownCommunication();
};
'''+r'''
// Register dispatch itself is tested against the production owner separately.
class ServiceRegisterIO:public cirrus::core::RegisterIO {
 Amp& amp;
public:
 ServiceRegisterIO(CirrusAudioFixup*,Amp& a):amp(a){}
 int32_t lastError()const override{return amp.registerIO->lastError();}
 bool read(uint32_t r,uint32_t* v)override{return amp.registerIO->read(r,v);}
 bool write(uint32_t r,uint32_t v)override{return amp.registerIO->write(r,v);}
 bool updateBits(uint32_t r,uint32_t m,uint32_t v)override{return amp.registerIO->updateBits(r,m,v);}
 bool bulkRead(uint32_t r,uint8_t* v,size_t n)override{return amp.registerIO->bulkRead(r,v,n);}
 bool bulkWrite(uint32_t r,const uint8_t* v,size_t n)override{return amp.registerIO->bulkWrite(r,v,n);}
 bool pollBit(uint32_t r,uint32_t m,uint32_t v,uint32_t t)override{return amp.registerIO->pollBit(r,m,v,t);}
};
using cirrus::diagnostics::DIAG_DEVICE_ID;
'''+owner_function('preflightI2CIdentity')+owner_function('recordAttachmentFailure')+owner_function('publishAttachmentStatus')+owner_function('createI2CController')+owner_function('bindAmplifierDrivers')+owner_function('releaseAmplifierDrivers')+owner_function('detectConfiguredI2CAmplifiers')+owner_function('setupCommunication')+owner_function('detectNativeSPIAmplifiers')+owner_function('teardownCommunication')+r'''
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

unsigned driversCreated=0,driversDestroyed=0;
int failStage=0;bool rejectFactory=false;
uint32_t fixtureIdentityRegister=0,fixtureIdentityMask=0xffffffffU;
bool access(uint32_t reg){return reg<=0x20;}
struct SPIHooks:cirrus::core::AmplifierLifecycleHooks {
 const cirrus::platform::AmplifierDriverContext& context;
 SPIHooks(const cirrus::platform::AmplifierDriverContext& c):context(c){
  ++driversCreated;
  if(context.host){assert(context.host->authorized()&&context.host->name());assert(!context.platformContext&&!context.platformContract);}
 }
 ~SPIHooks(){++driversDestroyed;}
 IOReturn identify() override {if(context.host)context.host->setDiagnosticStage(cirrus::diagnostics::STAGE_PROBE);uint32_t value=0;return context.io.read(fixtureIdentityRegister,&value)&&(value&fixtureIdentityMask)==context.board.amplifiers[context.bindingIndex].siliconId?0:kIOReturnNotFound;}
 IOReturn authorizeBoard() override {return context.board.permits(context.actualBoard)?0:kIOReturnNotPermitted;}
 IOReturn initialize() override {return context.io.write(4,1)?0:kIOReturnNotReady;}
 IOReturn loadFirmware() override {if(failStage==2)return kIOReturnNotReady;return context.io.bulkWrite(8,context.firmware.program,context.firmware.programBytes)?0:kIOReturnNotReady;}
 IOReturn verifyReady() override {if(context.host)context.host->markDiagnosticSuccess(cirrus::diagnostics::STAGE_SAFE_IDLE);return 0;}
 IOReturn startPlayback() override {if(failStage==3)return kIOReturnNotReady;return context.io.write(4,2)?0:kIOReturnNotReady;}
 IOReturn stopPlayback() override {return context.io.write(4,0)?0:kIOReturnNotReady;}
 IOReturn safeShutdown() override {if(failStage==4)return kIOReturnNotReady;return context.io.write(4,0)?0:kIOReturnNotReady;}
};
cirrus::core::AmplifierLifecycleHooks* createSPI(const cirrus::platform::AmplifierDriverContext& c){return rejectFactory?nullptr:new SPIHooks(c);}
void destroySPI(cirrus::core::AmplifierLifecycleHooks* h){delete h;}
// Independent communication owner: no Intel/AMD base class or register knowledge.
unsigned nativeCreates=0,nativeDestroys=0,nativeSuspends=0,nativeResumes=0;
bool rejectNative=false,failNativeResume=false;
class FixtureNativeSPI final : public cirrus::transport::NativeSPIControllerOwner,public cirrus::transport::SPIController {
 bool active=true;
public:
 FixtureNativeSPI(){++nativeCreates;}~FixtureNativeSPI(){++nativeDestroys;}
 cirrus::transport::SPIController& controller()override{return *this;}
 void suspend()override{active=false;++nativeSuspends;}
 IOReturn resume(cirrus::transport::NativeSPIControllerFailure& failure)override{++nativeResumes;active=!failNativeResume;failure={"CUSTOM_OWNER_RESUME_FAILED",active?0:kIOReturnNotReady,0};return failure.result;}
 size_t maxTransferBytes()const override{return 262;}
 IOReturn exchange(const cirrus::transport::SPIEndpoint&,const uint8_t*,uint8_t*,size_t)override{return kIOReturnUnsupported;}
 IOReturn command(const cirrus::transport::SPIEndpoint& endpoint,const uint8_t* tx,size_t tn,uint8_t* rx,size_t n)override{
  if(!active)return kIOReturnNotReady;
  if(n)for(size_t i=0;i<n;++i)rx[i]=n==4?uint8_t(probeIDs[endpoint.chipSelect]>>(24-8*i)):0;
  else {++writeTransfers;writes.emplace_back(tx,tx+tn);}return 0;
 }
};
cirrus::transport::NativeSPIControllerOwner* createCustomSPI(IOACPIPlatformDevice*,IOPCIDevice*,IOService*,cirrus::transport::NativeSPIControllerFailure& failure){
 if(rejectNative){failure={"CUSTOM_OWNER_FACTORY_REJECTED",kIOReturnUnsupported,7};return nullptr;}
 return new FixtureNativeSPI;
}


// Independently implemented provider-backed communication: no Voodoo subclass
// and no AMP knowledge. The same service/map/driver lifecycle consumes it.
unsigned customCreates=0,customDestroys=0,customTransfers=0;
bool rejectCustomController=false;
struct CustomI2C final : cirrus::transport::I2CController {
 CustomI2C(){++customCreates;}~CustomI2C(){++customDestroys;}
 size_t maxWriteBytes()const override{return 32;}
 size_t maxReadBytes()const override{return 32;}
 IOReturn transfer(const cirrus::transport::I2CEndpoint& ep,const uint8_t* tx,size_t tn,uint8_t* rx,size_t n)override {
  assert(ep.address==0x40&&!ep.tenBit&&tn<=32&&n<=32);++customTransfers;
  if(i2cResult)return i2cResult;
  if(!n)i2cWrites.emplace_back(tx,tx+tn);else i2cReads.emplace_back(tx,tx+tn);
  for(size_t i=0;i<n;++i)rx[i]=uint8_t(i2cProbeID>>(24-8*i));return 0;
 }
};
cirrus::transport::I2CController* createCustomI2C(IOService* provider){
 assert(provider&&!strcmp(provider->path.c_str(),"fixture-i2c-controller"));
 return rejectCustomController?nullptr:new CustomI2C;
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
 assert(!strcmp(host.failure().reason,"CONTROLLER_RESUME_FAILED")&&host.failure().result==kIOReturnNotReady);
 resumeFailure=false;b.power=3;assert(host.resume()==kIOReturnNotReady);b.power=0;assert(host.resume()==0&&host.registers(2)->read(0,&value));
 host.shutdown();host.shutdown();assert(!liveControllers&&a.refs==1&&b.refs==1&&!iteratorLive);
 resources(amp,{{"SPI0",0}});amp.crs.bytes[4]=1;assert(host.bind(&amp,&owner,format)==kIOReturnUnsupported&&!liveControllers);
 pci.acpi=nullptr;pci.acpiPath.value=a.path;resources(amp,{{"SPI0",0}});
 assert(host.bind(&amp,&owner,format)==0&&host.count()==1);host.shutdown();
 pci.acpi=&a;pci.acpiPath.value.clear();
 pci.supported=false;assert(host.bind(&amp,&owner,format)==kIOReturnUnsupported&&!liveControllers);
 assert(!strcmp(host.failure().reason,"INTEL_CONTROLLER_ID_UNSUPPORTED")&&host.failure().detail==0x80867aaa);pci.supported=true;
 resources(amp,{{"MISSING",0}});assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!host.bound()&&!liveControllers);
 assert(!strcmp(host.failure().reason,"CONTROLLER_ACPI_PATH_UNRESOLVED_OR_AMBIGUOUS")&&!strcmp(host.failure().source,"MISSING"));
 host.shutdown();assert(!strcmp(host.failure().source,"MISSING")&&host.failure().result==kIOReturnNotFound);
 resources(amp,{{"SPI0",0},{"SPI1",1}});b.hasPSC=false;
 assert(host.bind(&amp,&owner,format)==kIOReturnNotReady&&!liveControllers&&a.refs==1&&b.refs==1);b.hasPSC=true;
 b.amd=false;
 assert(host.bind(&amp,&owner,format)==kIOReturnUnsupported&&!host.bound()&&!liveControllers&&a.refs==1&&b.refs==1);
 assert(!strcmp(host.failure().reason,"AMD_CONTROLLER_HID_UNSUPPORTED")&&host.failure().index==1&&!strcmp(host.failure().source,"SPI1"));b.amd=true;
 resources(amp,{{"SPI0",0},{"SPI0",0}});assert(host.bind(&amp,&owner,format)==kIOReturnBadArgument&&!liveControllers);
 assert(!strcmp(host.failure().reason,"DUPLICATE_CHIP_SELECT")&&host.failure().index==1);
 resources(amp,{{"SPI0",0},{"\\_SB.SPI0",0}});assert(host.bind(&amp,&owner,format)==kIOReturnBadArgument&&!liveControllers);
 resources(amp,{{"SPI0",0}});amp.status=0;assert(host.bind(&amp,&owner,format)==kIOReturnNotReady);amp.status=3;
 amp.rejectCRS=true;assert(host.bind(&amp,&owner,format)==kIOReturnNotFound);amp.rejectCRS=false;
 iteratorFailure=true;assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!liveControllers);iteratorFailure=false;
 IOACPIPlatformDevice duplicate;duplicate.path=a.path;nodes.push_back(&duplicate);
 assert(host.bind(&amp,&owner,format)==kIOReturnNotFound&&!liveControllers&&a.refs==1);nodes.pop_back();
 assert(host.failure().result==kIOReturnNotFound);
 format.addressBytes=1;format.valueBytes=1;format.stride=1;format.paddingBytes=0;format.readFlag=0;format.maxRegister=255;
 assert(host.bind(&amp,&owner,format)==0&&!strcmp(host.failure().reason,"NONE"));uint8_t bytes[3]{};assert(host.registers(0)->bulkRead(1,bytes,3));
 host.shutdown();assert(!liveControllers&&!iteratorLive&&amp.crs.refs==1);
 CirrusAudioFixup gated;IOService i2c;gated.mProvider=&i2c;
 cirrus::platform::AmplifierDriverRegistration legacyDefinition{"fixture-legacy-spi","CSC3551",0x35a40,2,{},cirrus::devices::amplifiers::cs35l41::CS35L41SPIRegisterIO::config(),createSPI,destroySPI,true,0,0xffffffffU,cirrus::core::AmplifierModel::CS35L41};
 legacyDefinition.spiFormat.readable=legacyDefinition.spiFormat.writable=[](uint32_t){return true;};
 assert(gated.mDriverRegistry.add(legacyDefinition));
 assert(gated.setupCommunication(&i2c)&&gated.strings["Cirrus_Bus"]=="I2C");
 assert(!gated.setupCommunication(&amp)&&!gated.mNativeSPI.bound());
 gated.flags["-cirrusspi"]=true;assert(gated.setupCommunication(&amp)&&!gated.mPlatformAuthorized);gated.teardownCommunication();
 gated.flags["-cirrusro"]=true;assert(gated.setupCommunication(&amp)&&!gated.mPlatformAuthorized);
 assert(gated.strings["Cirrus_SPI_Status"]=="BOUND_READ_ONLY");
 assert(!gated.mNativeSPI.registers(0)->write(0,1));gated.teardownCommunication();assert(!liveControllers);

 amp.crs.bytes.clear();
 for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){auto d=descriptor("SPI0",i);amp.crs.bytes.insert(amp.crs.bytes.end(),d.begin(),d.end());}
 amp.crs.bytes.push_back(0x79);amp.crs.bytes.push_back(0);
 assert(host.bind(&amp,&owner,format)==0&&host.count()==ACPISPIResources::kMaxEndpoints&&liveControllers==1);host.shutdown();
 assert(!liveControllers&&a.refs==1);
 // Actual shipping discovery must preserve resource identity after a failed probe.
 CirrusAudioFixup service;assert(service.mDriverRegistry.add(legacyDefinition));format={};format.paddingBytes=2;format.readFlag=0x80;
 resources(amp,{{"SPI0",1},{"SPI0",0},{"SPI1",2}});
 probeIDs[0]=probeIDs[2]=0x35a40;probeIDs[1]=0xdead;
 assert(service.mNativeSPI.bind(&amp,&service,format)==0);
 assert(service.detectNativeSPIAmplifiers()==2);
 assert(service.mAmplifiers[0].endpointIndex==1&&service.mAmplifiers[0].endpoint.spiChipSelect==0);
 assert(!strcmp(service.mAmplifiers[0].name,"spi1")&&service.mAmplifiers[0].address==0xff);
 assert(service.mAmplifiers[0].channel==cirrus::core::AudioChannel::MaxChannels);
 assert(service.mAmplifiers[1].endpointIndex==2&&!strcmp(service.mAmplifiers[1].name,"spi2"));
 assert(service.numbers["Cirrus_SPI_0_DeviceID"]==0xdead&&service.numbers["Cirrus_SPI_Resource_Count"]==3);
 service.teardownCommunication();assert(!service.mAmplifiers[0].registerIO&&!liveControllers&&!iteratorLive);

 // Platform-selected native owner factory shares targets, owns PM and does not
 // fall back to built-in controllers after rejection. No host silicon branch.
 auto customProfile=*gated.mPlatformProfile;customProfile.spiControllerFactory=createCustomSPI;
 CirrusAudioFixup customSPI;customSPI.mPlatformProfile=&customProfile;customSPI.flags["-cirrusspi"]=true;
 assert(customSPI.mDriverRegistry.add(legacyDefinition));resources(amp,{{"SPI0",0},{"SPI0",1},{"SPI1",2}});
 const auto originalIntelResumes=resumes;
 assert(customSPI.setupCommunication(&amp) && nativeCreates==2 && !liveControllers);
 customSPI.suspendCommunication();assert(!strcmp(customSPI.mAttachment.communication,"SUSPENDED"));failNativeResume=true;
 assert(!customSPI.resumeCommunication() && nativeResumes==1 && nativeSuspends==4);
 assert(!strcmp(customSPI.mAttachment.first.stage,"CONTROLLER_PM") && !strcmp(customSPI.mAttachment.first.indexKind,"CONTROLLER_SLOT") && customSPI.mAttachment.first.result==kIOReturnNotReady);
 assert(!strcmp(customSPI.mNativeSPI.failure().reason,"CUSTOM_OWNER_RESUME_FAILED") && resumes==originalIntelResumes);
 failNativeResume=false;assert(customSPI.resumeCommunication() && nativeResumes==3 && customSPI.mAttachment.first.result==kIOReturnNotReady);
 customSPI.teardownCommunication();assert(nativeCreates==nativeDestroys && a.refs==1 && b.refs==1);
 rejectNative=true;const auto createdBefore=nativeCreates;
 assert(!customSPI.setupCommunication(&amp) && nativeCreates==createdBefore && !liveControllers && !customSPI.mNativeSPI.bound());
 assert(!strcmp(customSPI.mNativeSPI.failure().reason,"CUSTOM_OWNER_FACTORY_REJECTED") && customSPI.mNativeSPI.failure().detail==7);
 rejectNative=false;customSPI.teardownCommunication();assert(a.refs==1 && b.refs==1);
 // Full production native-controller -> register-map -> driver -> group path.
 using namespace cirrus::platform;
 resources(amp,{{"SPI1",2},{"SPI0",0}});amp.hid.value="TESTAMP";
 probeIDs[0]=0x1111;probeIDs[2]=0x2222;
 auto f1=format;f1.maxRegister=0x20;f1.readable=access;f1.writable=access;
 auto f2=f1;f2.addressBytes=2;f2.paddingBytes=1;f2.addressEndian=cirrus::core::RegisterEndian::Little;
 AmplifierDriverRegistration d1{"test-spi-1","TESTAMP",0x1111,2,{},f1,createSPI,destroySPI};
 AmplifierDriverRegistration d2{"test-spi-2","TESTAMP",0x2222,2,{},f2,createSPI,destroySPI};
 AmplifierDriverRegistry registry;assert(registry.add(d1)&&registry.add(d2));
 AmplifierBinding bindings[2]{{{cirrus::core::BusTransportType::SPI,"\\_SB.SPI0",0,false,0,1000000,0},0x1111,0,1,"tune-a","TESTAMP"},
                            {{cirrus::core::BusTransportType::SPI,"\\_SB.SPI1",0,false,2,1000000,0},0x2222,1,2,"tune-b","TESTAMP"}};
 BoardConfiguration board{{0x1234,0x5678,nullptr},bindings,2,true,true,true};
 const uint8_t program[]{1,2,3,4};
 cirrus::firmware::FirmwareBundle firmware[2]{{{0x1111,0x1234,0x5678,1,0,"tune-a"},cirrus::firmware::FirmwareFormat::Vendor,program,4},
                                           {{0x2222,0x1234,0x5678,2,1,"tune-b"},cirrus::firmware::FirmwareFormat::Vendor,program,4}};
 SPIAmplifierManager playback;
 unsigned before=writeTransfers;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0);
 assert(writeTransfers==before&&liveControllers==2&&amp.hid.refs==2);
 assert(playback.startPlayback()==kIOReturnNotReady);
 assert(playback.bringUp()==0&&writeTransfers==before+4);
 // Distinct formats, despite reversed ACPI resource/manifest order.
 assert(writes[writes.size()-4].size()==7&&writes[writes.size()-2].size()==10);
 assert(playback.runtime().registration(0)==&d2&&playback.runtime().registration(1)==&d1);
 assert(playback.startPlayback()==0&&playback.runtime().stage()==cirrus::core::AmplifierStage::Playing);
 assert(playback.suspend()==0);before=writeTransfers;
 assert(playback.startPlayback()==kIOReturnNotReady&&writeTransfers==before);
 resumeFailure=true;assert(playback.resume()==kIOReturnNotReady&&writeTransfers==before);
 resumeFailure=false;assert(playback.resume()==0&&writeTransfers>before&&playback.runtime().stage()==cirrus::core::AmplifierStage::Playing);
 before=writeTransfers;assert(playback.notifyPowerLost()==0&&writeTransfers==before);
 assert(playback.resume()==0&&playback.runtime().stage()==cirrus::core::AmplifierStage::Ready);
 assert(playback.startPlayback()==0);
 assert(playback.unbind()==0&&!liveControllers&&amp.hid.refs==1&&driversCreated==driversDestroyed);
 before=writeTransfers;board.powerVerified=false;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)!=0&&writeTransfers==before&&!liveControllers);board.powerVerified=true;
 firmware[1].target.speakerId=3;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)!=0&&writeTransfers==before&&!liveControllers&&driversCreated==driversDestroyed);firmware[1].target.speakerId=2;
 rejectFactory=true;assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)!=0&&!liveControllers&&writeTransfers==before);rejectFactory=false;
 probeIDs[2]=0xdead;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0);
 assert(playback.bringUp()==kIOReturnNotFound&&writeTransfers==before);playback.unbind();probeIDs[2]=0x2222;
 failStage=2;assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0);
 assert(playback.bringUp()==kIOReturnNotReady&&playback.runtime().stage()==cirrus::core::AmplifierStage::Faulted);
 playback.unbind();failStage=0;assert(!liveControllers&&driversCreated==driversDestroyed&&amp.hid.refs==1);

 failStage=3;assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0&&playback.bringUp()==0);
 assert(playback.startPlayback()==kIOReturnNotReady&&playback.runtime().stage()==cirrus::core::AmplifierStage::Faulted);
 playback.unbind();failStage=0;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0&&playback.bringUp()==0&&playback.startPlayback()==0);
 auto previousSuspends=suspends;failStage=4;
 assert(playback.suspend()==kIOReturnNotReady&&suspends==previousSuspends&&liveControllers==2);
 assert(playback.runtime().status(0)->cleanupResult==kIOReturnNotReady);
 assert(playback.unbind()==kIOReturnNotReady&&!liveControllers&&playback.runtime().releasedStatus(0)->cleanupResult==kIOReturnNotReady);failStage=0;
 before=writeTransfers;bindings[1].endpoint.spiClockHz=500000;
 assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==kIOReturnNotPermitted&&!liveControllers&&writeTransfers==before);bindings[1].endpoint.spiClockHz=1000000;
 amp.hid.value.clear();assert(playback.bind(&amp,&owner,registry,board,board.identity,firmware,2)==kIOReturnNotFound&&!liveControllers);amp.hid.value="TESTAMP";
 {SPIAmplifierManager scoped;assert(scoped.bind(&amp,&owner,registry,board,board.identity,firmware,2)==0&&scoped.bringUp()==0&&scoped.startPlayback()==0);}
 assert(!liveControllers&&driversCreated==driversDestroyed&&amp.hid.refs==1);
 // Topology failure must preserve discovery-only ports atomically.
 assert(host.bind(&amp,&owner,format)==0);bindings[1].endpoint.spiMode=1;
 assert(host.configureAuthorized(registry,board,board.identity,"TESTAMP")==kIOReturnNotPermitted);
 assert(!host.registers(0)->write(4,1)&&!host.registers(1)->write(4,1));bindings[1].endpoint.spiMode=0;host.shutdown();

 // Registry-only AMP extension through the actual IOService: the two arbitrary
 // IDs have no Core/AmplifierDescriptors entry and different framing. This is
 // host fixture evidence, never a newly supported shipping silicon/board.
 assert(!cirrus::core::findAmplifierById(0x1111)&&!cirrus::core::findAmplifierById(0x2222));
 BoardConfiguration extensionBoard=board;extensionBoard.identity.acpiHid="TESTAMP";
 cirrus::platform::PlatformProfile extensionProfile{"fixture-registry-only","TESTAMP",cirrus::core::AmplifierModel::Unknown,
     nullptr,0,nullptr,true,&extensionBoard,firmware,2};
 CirrusAudioFixup extension;extension.mProvider=&amp;extension.mPlatformProfile=&extensionProfile;extension.mPlatformSubsystemId=0x12345678;
 assert(extension.mDriverRegistry.add(d1)&&extension.mDriverRegistry.add(d2));extension.flags["-cirrusspi"]=true;
 before=writeTransfers;
 assert(extension.setupCommunication(&amp)&&extension.detectNativeSPIAmplifiers()==2&&writeTransfers==before);
 assert(extension.mAmplifiers[0].deviceId==0x2222&&extension.mAmplifiers[1].deviceId==0x1111);
 assert(extension.mAmplifiers[0].model==cirrus::core::AmplifierModel::Unknown);
 assert(extension.bindAmplifierDrivers()&&writeTransfers==before);
 assert(extension.mAmplifierManager.bringUp()==0&&extension.mAmplifierManager.startPlayback()==0);
 assert(extension.hostReports==4);
 assert(extension.mAmplifierManager.registration(0)==&d2&&extension.mAmplifierManager.registration(1)==&d1);
 assert(extension.mAmplifierManager.suspend()==0&&extension.mAmplifierManager.resume()==0);
 extension.teardownCommunication();assert(!liveControllers&&driversCreated==driversDestroyed);
 // Missing registration and wrong identity are fail-closed during discovery.
 CirrusAudioFixup unregistered;unregistered.mProvider=&amp;unregistered.mPlatformProfile=&extensionProfile;unregistered.mPlatformSubsystemId=0x12345678;
 assert(unregistered.mNativeSPI.bind(&amp,&unregistered,f1)==0);
 before=writeTransfers;assert(unregistered.detectNativeSPIAmplifiers()==0&&writeTransfers==before);
 unregistered.teardownCommunication();assert(!liveControllers);
 CirrusAudioFixup mismatched;mismatched.mProvider=&amp;mismatched.mPlatformProfile=&extensionProfile;
 assert(mismatched.mDriverRegistry.add(d1)&&mismatched.mDriverRegistry.add(d2));
 assert(mismatched.mNativeSPI.bind(&amp,&mismatched,f1)==0);probeIDs[2]=0x35a40;
 before=writeTransfers;assert(mismatched.detectNativeSPIAmplifiers()==1&&writeTransfers==before);
 assert(mismatched.mAmplifiers[0].deviceId==0x1111);probeIDs[2]=0x2222;
 mismatched.teardownCommunication();assert(!liveControllers);
 // Actual IOService binder, including firmware selection and resource ordering.
 amp.hid.value="CSC3551";probeIDs[0]=probeIDs[2]=0x35a40;
 AmplifierBinding shipBindings[2]{bindings[0],bindings[1]};
 for(auto& binding:shipBindings){binding.siliconId=0x35a40;binding.acpiHid="CSC3551";}
 BoardConfiguration shipBoard{{0x1234,0x5678,"CSC3551"},shipBindings,2,true,true,true};
 cirrus::firmware::FirmwareBundle shipFirmware[2]{firmware[0],firmware[1]};
 for(auto& asset:shipFirmware)asset.target.siliconId=0x35a40;
 cirrus::platform::PlatformProfile shipProfile{"fixture-spi","CSC3551",cirrus::core::AmplifierModel::CS35L41,nullptr,0,nullptr,true,&shipBoard,shipFirmware,2};
 AmplifierDriverRegistration shipDefinition{"fixture-service","CSC3551",0x35a40,2,{},f1,createSPI,destroySPI};
 CirrusAudioFixup shipping;shipping.mProvider=&amp;shipping.mPlatformProfile=&shipProfile;shipping.mPlatformSubsystemId=0x12345678;
 assert(shipping.mDriverRegistry.add(shipDefinition));shipping.flags["-cirrusspi"]=true;
 before=writeTransfers;
 assert(shipping.setupCommunication(&amp)&&shipping.mPlatformAuthorized&&shipping.detectNativeSPIAmplifiers()==2);
 assert(shipping.mAmplifiers[0].channel==cirrus::core::AudioChannel::Right&&shipping.mAmplifiers[1].channel==cirrus::core::AudioChannel::Left);
 assert(shipping.bindAmplifierDrivers()&&writeTransfers==before);
 assert(shipping.mAmplifierManager.bringUp()==0&&shipping.mAmplifierManager.startPlayback()==0);
 shipping.teardownCommunication();assert(!shipping.mAmplifierManager.bound()&&!liveControllers&&driversCreated==driversDestroyed);
 CirrusAudioFixup verifiedReadOnly;verifiedReadOnly.mProvider=&amp;verifiedReadOnly.mPlatformProfile=&shipProfile;verifiedReadOnly.mPlatformSubsystemId=0x12345678;
 assert(verifiedReadOnly.mDriverRegistry.add(shipDefinition));verifiedReadOnly.flags["-cirrusspi"]=verifiedReadOnly.flags["-cirrusro"]=true;
 assert(verifiedReadOnly.setupCommunication(&amp)&&!verifiedReadOnly.mPlatformAuthorized);
 before=writeTransfers;
 assert(!verifiedReadOnly.mNativeSPI.registers(0)->write(4,1)&&!verifiedReadOnly.bindAmplifierDrivers()&&writeTransfers==before);
 verifiedReadOnly.teardownCommunication();assert(!liveControllers);
 // Actual configured I2C discovery and binder use this driver's one-byte
 // address format, rather than silently imposing CS35L41 four-byte framing.
 IOService i2cProvider;i2cProvider.path="fixture-i2c-controller";
 AmplifierBinding i2cBinding{{cirrus::core::BusTransportType::I2C,"fixture-i2c-controller",0x40},0x35a40,0,1,"tune-a","CSC3551"};
 BoardConfiguration i2cBoard{{0x1234,0x5678,"CSC3551"},&i2cBinding,1,true,true,true};
 cirrus::platform::PlatformProfile i2cProfile{"fixture-i2c","CSC3551",cirrus::core::AmplifierModel::CS35L41,nullptr,0,nullptr,true,&i2cBoard,shipFirmware,1};
 auto i2cFormat=f1;i2cFormat.addressBytes=1;i2cFormat.paddingBytes=0;i2cFormat.readFlag=0;
 AmplifierDriverRegistration i2cDefinition{"fixture-i2c","CSC3551",0x35a40,1,i2cFormat,{},createSPI,destroySPI};
 CirrusAudioFixup i2cShipping;i2cShipping.mProvider=&i2cProvider;i2cShipping.mPlatformProfile=&i2cProfile;i2cShipping.mPlatformSubsystemId=0x12345678;
 assert(i2cShipping.mDriverRegistry.add(i2cDefinition));
 assert(i2cShipping.detectConfiguredI2CAmplifiers()==1&&i2cWrites.empty());
 assert(i2cShipping.bindAmplifierDrivers()&&i2cWrites.empty());
 assert(i2cShipping.mAmplifierManager.bringUp()==0&&i2cShipping.mAmplifierManager.startPlayback()==0);
 assert(i2cWrites.size()==3&&i2cWrites[0].size()==5&&i2cWrites[0][0]==4);
 i2cShipping.teardownCommunication();assert(!i2cShipping.mAmplifiers[0].registerIO&&!i2cShipping.mI2CController&&driversCreated==driversDestroyed);
 CirrusAudioFixup i2cReadOnly;i2cReadOnly.mProvider=&i2cProvider;i2cReadOnly.mPlatformProfile=&i2cProfile;i2cReadOnly.mPlatformSubsystemId=0x12345678;i2cReadOnly.flags["-cirrusro"]=true;
 assert(i2cReadOnly.mDriverRegistry.add(i2cDefinition)&&i2cReadOnly.detectConfiguredI2CAmplifiers()==1);
 auto i2cBefore=i2cWrites.size();
 assert(!i2cReadOnly.mAmplifiers[0].registerIO->write(4,1)&&!i2cReadOnly.bindAmplifierDrivers()&&i2cWrites.size()==i2cBefore);
 assert(i2cReadOnly.detectConfiguredI2CAmplifiers()==1&&i2cWrites.size()==i2cBefore);
 assert(!i2cReadOnly.mAmplifiers[0].registerIO->write(4,1));
 i2cReadOnly.teardownCommunication();assert(!i2cReadOnly.mAmplifiers[0].registerIO&&!i2cReadOnly.mI2CController);

 // The same extension boundary on I2C, including a masked vendor ID at a
 // nonzero identity register and one-byte addresses. No central descriptor.
 assert(!cirrus::core::findAmplifierById(0x3333));
 i2cProbeID=0xa5003333;fixtureIdentityRegister=0x10;fixtureIdentityMask=0xffff;
 AmplifierBinding newI2CBinding{{cirrus::core::BusTransportType::I2C,"fixture-i2c-controller",0x40},0x3333,0,1,"tune-new","CSC3551"};
 BoardConfiguration newI2CBoard{{0x1234,0x5678,"CSC3551"},&newI2CBinding,1,true,true,true};
 cirrus::firmware::FirmwareBundle newI2CFirmware{{0x3333,0x1234,0x5678,1,0,"tune-new"},cirrus::firmware::FirmwareFormat::Vendor,program,4};
 cirrus::platform::PlatformProfile newI2CProfile{"fixture-new-i2c","CSC3551",cirrus::core::AmplifierModel::Unknown,nullptr,0,nullptr,true,&newI2CBoard,&newI2CFirmware,1};
 AmplifierDriverRegistration newI2CDefinition{"fixture-new-i2c","CSC3551",0x3333,1,i2cFormat,{},createSPI,destroySPI,false,0x10,0xffff};
 CirrusAudioFixup newI2C;newI2C.mProvider=&i2cProvider;newI2C.mPlatformProfile=&newI2CProfile;newI2C.mPlatformSubsystemId=0x12345678;
 newI2CProfile.i2cControllerFactory=createCustomI2C;
 assert(newI2C.mDriverRegistry.add(newI2CDefinition));i2cBefore=i2cWrites.size();
 assert(newI2C.detectConfiguredI2CAmplifiers()==1&&i2cWrites.size()==i2cBefore);
 assert(newI2C.mAmplifiers[0].deviceId==0x3333&&newI2C.mAmplifiers[0].model==cirrus::core::AmplifierModel::Unknown);
 assert(i2cReads.back().size()==1&&i2cReads.back()[0]==0x10);
 auto* discoveryController=newI2C.mI2CController;
 assert(customCreates==1&&customDestroys==0&&customTransfers==1);
 assert(newI2C.bindAmplifierDrivers()&&i2cWrites.size()==i2cBefore&&newI2C.mI2CController==discoveryController&&customCreates==1);
 const auto boundTransfers=customTransfers;
 assert(newI2C.detectConfiguredI2CAmplifiers()==1&&newI2C.mI2CController==discoveryController&&customTransfers==boundTransfers);
 assert(newI2C.mAmplifierManager.bringUp()==0&&newI2C.mAmplifierManager.startPlayback()==0);
 assert(newI2C.hostReports==2);
 assert(newI2C.mAmplifierManager.suspend()==0&&newI2C.mAmplifierManager.resume()==0);
 newI2C.teardownCommunication();assert(!newI2C.mI2CController&&driversCreated==driversDestroyed&&customCreates==customDestroys);
 CirrusAudioFixup factoryRejected;factoryRejected.mProvider=&i2cProvider;factoryRejected.mPlatformProfile=&newI2CProfile;factoryRejected.mPlatformSubsystemId=0x12345678;
 assert(factoryRejected.mDriverRegistry.add(newI2CDefinition));rejectCustomController=true;i2cBefore=i2cWrites.size();
 const auto readsBefore=i2cReads.size();assert(!factoryRejected.createI2CController());assert(factoryRejected.detectConfiguredI2CAmplifiers()==0&&!factoryRejected.mI2CController);
 assert(!factoryRejected.bindAmplifierDrivers()&&i2cReads.size()==readsBefore&&i2cWrites.size()==i2cBefore);
 factoryRejected.teardownCommunication();rejectCustomController=false;

 // Matrix: each CPU family uses the same AMP/runtime contract. SPI controller
 // selection follows real provider resources (PCI vs ACPI), not a CPU switch.
 fixtureIdentityRegister=0;fixtureIdentityMask=0xffffffffU;i2cProbeID=probeIDs[0]=0x35a40;
 for(const char* cpu:{"INTEL","AMD"}){
  const bool isIntel=!strcmp(cpu,"INTEL");pciNodes=isIntel?std::vector<IORegistryEntry*>{&pci}:std::vector<IORegistryEntry*>{};a.amd=!isIntel;
  resources(amp,{{"SPI0",0}});
  AmplifierBinding oneSPIBinding{{cirrus::core::BusTransportType::SPI,"\\_SB.SPI0",0,false,0,1000000,0},0x35a40,0,1,"tune-a","CSC3551"};
  BoardConfiguration oneSPIBoard{{0x1234,0x5678,"CSC3551"},&oneSPIBinding,1,true,true,true};
  cirrus::platform::PlatformProfile oneSPIProfile{"matrix-spi","CSC3551",cirrus::core::AmplifierModel::CS35L41,nullptr,0,nullptr,true,&oneSPIBoard,shipFirmware,1};
  CirrusAudioFixup readySPI;readySPI.mPlatformProfile=&oneSPIProfile;readySPI.mPlatformSubsystemId=0x12345678;readySPI.flags["-cirrusspi"]=true;readySPI.setProperty("Cirrus_CPU_Vendor",cpu);
  assert(readySPI.mDriverRegistry.add(shipDefinition));assert(readySPI.setupCommunication(&amp) && readySPI.detectNativeSPIAmplifiers()==1);
  assert(readySPI.bindAmplifierDrivers() && readySPI.mAmplifierManager.bringUp()==0 && readySPI.mAmplifierManager.startPlayback()==0);
  assert(!strcmp(readySPI.mAttachment.communication,"READY") && readySPI.mAttachment.failureCount==0);
  readySPI.teardownCommunication();assert(!liveControllers);
  CirrusAudioFixup unsupportedAMP;unsupportedAMP.mPlatformProfile=&oneSPIProfile;unsupportedAMP.flags["-cirrusspi"]=true;
  auto writesBefore=writeTransfers;assert(unsupportedAMP.setupCommunication(&amp));
  assert(unsupportedAMP.mNativeSPI.count()==1 && !unsupportedAMP.mNativeSPI.registers(0));
  assert(!strcmp(unsupportedAMP.mAttachment.communication,"READY") && !strcmp(unsupportedAMP.mAttachment.amplifier,"UNSUPPORTED"));
  assert(!strcmp(unsupportedAMP.mAttachment.first.stage,"AMPLIFIER") && writeTransfers==writesBefore && !unsupportedAMP.bindAmplifierDrivers());
  assert(unsupportedAMP.mNativeSPI.configureAuthorized(readySPI.mDriverRegistry,oneSPIBoard,oneSPIBoard.identity,"CSC3551")==kIOReturnUnsupported);
  unsupportedAMP.teardownCommunication();assert(!liveControllers);
  CirrusAudioFixup unsupportedCommunication;unsupportedCommunication.mPlatformProfile=&oneSPIProfile;unsupportedCommunication.flags["-cirrusspi"]=true;
  assert(unsupportedCommunication.mDriverRegistry.add(shipDefinition));if(isIntel)pci.supported=false;else a.amd=false;
  assert(!unsupportedCommunication.setupCommunication(&amp));
  assert(!strcmp(unsupportedCommunication.mAttachment.amplifier,"REGISTERED_NOT_PROBED"));
  assert(!strcmp(unsupportedCommunication.mAttachment.first.stage,"COMMUNICATION") && unsupportedCommunication.mAttachment.first.result==kIOReturnUnsupported);
  assert(!liveControllers && writeTransfers==writesBefore);pci.supported=true;a.amd=!isIntel;
  CirrusAudioFixup readyI2C;readyI2C.mProvider=&i2cProvider;readyI2C.mPlatformProfile=&i2cProfile;readyI2C.mPlatformSubsystemId=0x12345678;readyI2C.setProperty("Cirrus_CPU_Vendor",cpu);
  assert(readyI2C.mDriverRegistry.add(i2cDefinition));assert(readyI2C.setupCommunication(&i2cProvider) && readyI2C.detectConfiguredI2CAmplifiers()==1 && readyI2C.bindAmplifierDrivers());
  assert(readyI2C.mAmplifierManager.bringUp()==0 && readyI2C.mAmplifierManager.startPlayback()==0);readyI2C.teardownCommunication();
  CirrusAudioFixup unsupportedI2C;unsupportedI2C.mProvider=&i2cProvider;unsupportedI2C.mPlatformProfile=&i2cProfile;unsupportedI2C.mPlatformSubsystemId=0x12345678;
  assert(unsupportedI2C.mDriverRegistry.add(i2cDefinition));i2cProvider.addressedABI=false;auto readsBefore=i2cReads.size();auto i2cWritesBefore=i2cWrites.size();
  assert(unsupportedI2C.detectConfiguredI2CAmplifiers()==0 && i2cReads.size()==readsBefore && i2cWrites.size()==i2cWritesBefore);
  assert(!strcmp(unsupportedI2C.mAttachment.first.reason,"I2C_PROVIDER_ABI_UNAVAILABLE") && !strcmp(unsupportedI2C.mAttachment.amplifier,"REGISTERED_NOT_PROBED"));
  unsupportedI2C.teardownCommunication();i2cProvider.addressedABI=true;
  CirrusAudioFixup wrongIdentity;wrongIdentity.mProvider=&i2cProvider;wrongIdentity.mPlatformProfile=&i2cProfile;wrongIdentity.mPlatformSubsystemId=0x12345678;
  assert(wrongIdentity.mDriverRegistry.add(i2cDefinition));i2cProbeID=0xdead;
  assert(wrongIdentity.detectConfiguredI2CAmplifiers()==0 && i2cWrites.size()==i2cWritesBefore);
  assert(!strcmp(wrongIdentity.mAttachment.communication,"READY") && !strcmp(wrongIdentity.mAttachment.first.reason,"AMP_IDENTITY_MISMATCH"));wrongIdentity.teardownCommunication();i2cProbeID=0x35a40;
  CirrusAudioFixup missingFunction;missingFunction.mProvider=&i2cProvider;missingFunction.mPlatformProfile=&i2cProfile;missingFunction.mPlatformSubsystemId=0x12345678;
  assert(missingFunction.mDriverRegistry.add(i2cDefinition));i2cResult=kIOReturnUnsupported;
  assert(!missingFunction.setupCommunication(&i2cProvider) && missingFunction.mAttachment.first.result==kIOReturnUnsupported);
  assert(!strcmp(missingFunction.mAttachment.communication,"UNSUPPORTED") && i2cWrites.size()==i2cWritesBefore);missingFunction.teardownCommunication();i2cResult=0;
 }
 pciNodes={&pci};a.amd=false;
 // Direct ACPI I2C must not be guessed as SPI; no controller/AMP traffic.
 std::string source="SPI0";std::vector<uint8_t> i2cCRS(18+source.size()+1);i2cCRS[0]=0x8e;i2cCRS[1]=uint8_t(i2cCRS.size()-3);i2cCRS[3]=1;i2cCRS[5]=1;i2cCRS[6]=2;i2cCRS[9]=1;i2cCRS[10]=6;i2cCRS[16]=0x40;memcpy(i2cCRS.data()+18,source.c_str(),source.size()+1);i2cCRS.push_back(0x79);i2cCRS.push_back(0);
 amp.crs.bytes=i2cCRS;CirrusAudioFixup acpiI2C;const auto busWrites=writeTransfers;
 assert(!acpiI2C.setupCommunication(&amp) && acpiI2C.strings["Cirrus_Bus"]=="I2C" && writeTransfers==busWrites && !liveControllers);
 assert(!strcmp(acpiI2C.mAttachment.first.reason,"NATIVE_I2C_PROVIDER_CONTRACT_UNAVAILABLE"));
 assert(acpiI2C.numbers["Cirrus_Attach_First_Return"]==uint32_t(kIOReturnUnsupported) && acpiI2C.numbers["Cirrus_Attach_Latest_Return"]<=UINT32_MAX);
 // Selected ACPI I2C adapters obey the same pre-reset identity and AMP policy.
 auto originalACPIPath=amp.path;amp.path="fixture-i2c-controller";
 auto directBinding=i2cBinding;directBinding.endpoint.controllerPath=amp.path.c_str();
 auto directBoard=i2cBoard;directBoard.amplifiers=&directBinding;
 auto directProfile=i2cProfile;directProfile.boardConfiguration=&directBoard;directProfile.i2cControllerFactory=createCustomI2C;
 CirrusAudioFixup directReady;directReady.mProvider=&amp;directReady.mPlatformProfile=&directProfile;
 assert(directReady.mDriverRegistry.add(i2cDefinition));
 auto directReads=i2cReads.size();assert(directReady.setupCommunication(&amp) && i2cReads.size()==directReads+1 && directReady.mPlatformAuthorized);
 directReady.teardownCommunication();
 CirrusAudioFixup directUnsupported;directUnsupported.mProvider=&amp;directUnsupported.mPlatformProfile=&directProfile;
 assert(directUnsupported.mDriverRegistry.add(i2cDefinition));i2cResult=kIOReturnUnsupported;
 assert(!directUnsupported.setupCommunication(&amp) && !strcmp(directUnsupported.mAttachment.first.reason,"I2C_ADDRESSED_FUNCTION_UNSUPPORTED"));
 directUnsupported.teardownCommunication();i2cResult=0;
 CirrusAudioFixup directNoAMP;directNoAMP.mProvider=&amp;directNoAMP.mPlatformProfile=&directProfile;directReads=i2cReads.size();
 assert(directNoAMP.setupCommunication(&amp) && !directNoAMP.mPlatformAuthorized && i2cReads.size()==directReads);
 assert(!strcmp(directNoAMP.mAttachment.first.reason,"NO_REGISTERED_DRIVER"));directNoAMP.teardownCommunication();
 amp.path=originalACPIPath;
 auto mixedBus=descriptor("SPI0",0);mixedBus.insert(mixedBus.end(),i2cCRS.begin(),i2cCRS.end());amp.crs.bytes=mixedBus;
 CirrusAudioFixup mixedResources;assert(!mixedResources.setupCommunication(&amp) && !strcmp(mixedResources.mAttachment.first.reason,"MIXED_I2C_SPI_RESOURCES") && !liveControllers);
 puts("PASS Intel/AMD x I2C/SPI attachment/runtime matrix, unsupported controller/AMP separation, ACPI bus classification and no unsupported AMP writes");
 fixtureIdentityRegister=0;fixtureIdentityMask=0xffffffffU;i2cProbeID=0x35a40;
}
''')
 for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('PASS native SPI host: ACPI lookup, multi-controller ownership, authorized per-driver SPI formats, firmware/playback lifecycle, rollback and PM faults; hardware untested')
