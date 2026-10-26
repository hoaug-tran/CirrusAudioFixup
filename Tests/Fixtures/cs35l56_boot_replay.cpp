#include "ReplayOS.hpp"
#include "Platform/ACPI/ACPIObject.hpp"
#include "Platform/ACPI/ACPIEvaluation.hpp"
#include "Transport/SPI/NativeSPIHost.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56SPIRegisterIO.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Device.hpp"

using namespace cirrus::transport;
using namespace cirrus::transport::acpi;
constexpr int TRACE_PROBE=0;
KernelPatcher patcher;
struct KextInfo {size_t loadIndex=1;} acpiInfo[1];
#include "Callback.inc"

void replayAdvance(uint64_t target){
 assert(target>=nowNs);
 const uint64_t earlyAt=13323940000ULL;
 while(true){
  const auto nextEarly=earlyDone?UINT64_MAX:earlyAt;
  const auto nextCallback=callbackDone?UINT64_MAX:callbackAtNs;
  const auto next=std::min(nextEarly,nextCallback);
  if(next>target||next==UINT64_MAX)break;
  nowNs=std::max(nowNs,next);
  if(nextEarly<=nextCallback){earlyDone=true;if(earlyAction)earlyAction();}
  else {callbackDone=true;if(callbackAction)callbackAction();}
 }
 nowNs=target;
}
void* IOMalloc(size_t bytes){++allocations;return calloc(1,bytes);}
void IOFree(void* p,size_t){assert(allocations);--allocations;free(p);}

IOACPIPlatformDevice *spiNode=nullptr,*ampNode=nullptr;
uint32_t replayGetHandle(void*,const char* path,void** result){
 if(!strcmp(path,"\\_SB.PC00.SPI0"))*result=spiNode;
 else if(!strcmp(path,"\\_SB.PC00.SPI0.SPK1"))*result=ampNode;
 else return 5;
 return 0;
}
uint32_t replayGetName(void* handle,uint32_t type,Buffer* result){
 assert(handle==ampNode&&type==0);
 const char* name="\\_SB.PC00.SPI0.SPK1";size_t length=strlen(name)+1;assert(result->length>=length);
 memcpy(result->pointer,name,length);result->length=length;return 0;
}
uint32_t replayEvaluate(void* handle,const char* name,const void* args,Buffer* result){
 assert(handle==spiNode&&!strcmp(name,"_DSD")&&!args);++evaluations;
 struct DSD {Object root,pairs[2],property,items[2],cs[5];uint8_t uuid[16];char key[16];};
 assert(result->length>=sizeof(DSD));auto* p=static_cast<DSD*>(result->pointer);*p={};
 const uint8_t uuid[]={0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01};
 memcpy(p->uuid,uuid,16);strcpy(p->key,"cs-gpios");
 p->root.package={4,2,p->pairs};p->pairs[0].buffer={3,16,p->uuid};p->pairs[1].package={4,1,&p->property};
 p->property.package={4,2,p->items};p->items[0].string={2,8,p->key};p->items[1].package={4,5,p->cs};
 p->cs[0].integer={1,0};p->cs[1].reference={20,6,ampNode};
 for(unsigned i=2;i<5;++i)p->cs[i].integer={1,0};
 if(malformedDSD)p->cs[1].reference.actualType=1;
 result->length=sizeof(*p);return 0;
}
uintptr_t KernelPatcher::symbol(const char* name){
 if(missingSymbol)return 0;
 if(!strcmp(name,"_AcpiGetHandle"))return reinterpret_cast<uintptr_t>(replayGetHandle);
 if(!strcmp(name,"_AcpiGetName"))return reinterpret_cast<uintptr_t>(replayGetName);
 if(!strcmp(name,"_AcpiEvaluateObject"))return reinterpret_cast<uintptr_t>(replayEvaluate);
 assert(false);return 0;
}

struct IOTimerEventSource {uint64_t due=UINT64_MAX;void setTimeoutMS(unsigned ms){due=nowNs+uint64_t(ms)*1000000;}};
struct CirrusAudioFixup:IOService {
 using super=IOService;
 NativeSPIHost mNativeSPI;
 IOService* mProvider=nullptr;
 struct Profile {const char* acpiHid="CSC3556";bool allowAutomaticInitialization=true;
  const cirrus::platform::BoardConfiguration* boardConfiguration=nullptr;
  cirrus::core::AmplifierModel amplifierModel=cirrus::core::AmplifierModel::CS35L56;} profile;
 Profile* mPlatformProfile=&profile;
 struct Registry {
  cirrus::platform::AmplifierDriverRegistration definition{};
  bool acceptsDebugPhase(const char*,const char*){return false;}
  const cirrus::platform::AmplifierDriverRegistration* find(const char*,uint32_t,cirrus::core::BusTransportType){return &definition;}
  const cirrus::platform::AmplifierDriverRegistration* findModel(const char*,cirrus::core::AmplifierModel,cirrus::core::BusTransportType){return &definition;}
 } mDriverRegistry;
 struct Attachment {const char* amplifier="REGISTERED_NOT_PROBED";const char* communication="READY";struct {const char* stage="NONE";} latest;} mAttachment;
 static constexpr size_t kMaxAmplifiers=4;
 bool mProbingAmplifiers=false,mPlatformAuthorized=false,readOnly=true;
 struct Amp {char endpointLabel[16]{};const char* name=nullptr;uint8_t address=0,endpointIndex=0;
  cirrus::core::AudioChannel channel{};cirrus::core::BusEndpoint endpoint{};cirrus::core::RegisterIO* registerIO=nullptr;
  cirrus::core::AmplifierModel model{};bool present=false;uint32_t deviceId=0;} mAmplifiers[4];
 bool bootArgStrEquals(const char*,const char*){return false;}
 void recordTrace(int,uint8_t,bool,bool,uint32_t,uint32_t,IOReturn){}

 IOTimerEventSource timer;IOTimerEventSource* mProbeTimer=&timer;
 bool mStopping=false,mStartupPending=false,mStartupFailed=false,mPowerAvailable=true,mResumePending=false;
 bool mPlatformIdentityPending=false,mPMInitialized=false,probeDone=false;
 uint32_t mPlatformSubsystemId=0x10431c63;
 uint64_t mPlatformIdentityDeadlineMs=0;
 unsigned mAmplifierCount=0,finishes=0,failures=0;
 std::string firstFailure;
 bool bootArgEnabled(const char* arg){return readOnly&&!strcmp(arg,"-cirrusro");}
 bool registerAmplifierDrivers(){auto& d=mDriverRegistry.definition;d.siliconId=0x35a56;d.identityMask=0xffffff;
  d.spiFormat=cirrus::devices::amplifiers::cs35l56::CS35L56SPIRegisterIO::config();
  d.prepareIdentity=cirrus::devices::amplifiers::cs35l56::CS35L56Device::prepareIdentity;return true;}void resolvePlatformProfile(){}
 void logProviderInfo(IOService*){}void dumpProviderProperties(IOService*){}
 bool setupProbeTimer(){return true;}void publishAttachmentStatus(){}void publishDriverVerdict(){}
 uint64_t playbackTimeMilliseconds(){return nowNs/1000000;}
 bool setupCommunication(IOService* provider){
  // Adapter ends at the real NativeSPIHost; board/DSP lifecycle has its own suite.
  auto result=mNativeSPI.bound()?mNativeSPI.continueBind():mNativeSPI.bind(static_cast<IOACPIPlatformDevice*>(provider),this,
   cirrus::devices::amplifiers::cs35l56::CS35L56SPIRegisterIO::config());
  if(mNativeSPI.powerPending())return true;
  if(result){recordAttachmentFailure("COMMUNICATION",mNativeSPI.failure().reason,result);return false;}
  return true;
 }
 void teardownCommunication(){mNativeSPI.shutdown();}
 void stop(IOService*){mStopping=true;mStartupPending=false;timer.due=UINT64_MAX;teardownCommunication();stopped=true;}
 void recordAttachmentFailure(const char*,const char* reason,int,unsigned=0,const char* =""){
  if(firstFailure.empty())firstFailure=reason;++failures;
 }
 bool performPlatformHardwareReset(){++finishes;if(!readOnly)bootReadyAtNs=nowNs+resetBootDelayNs;return true;}
 unsigned expectedAmplifierCount(){return 2;}
 size_t detectNativeSPIAmplifiers();
 void detectAmplifiers(){assert(false);}void bringUpAmplifiers(){}
 void scheduleReadOnlyProbe(unsigned){timer.setTimeoutMS(100);}void setupAmplifierInterrupt(){}
 bool setupPowerManagement(IOService*){mPMInitialized=true;return true;}
 void runReadOnlyProbe(){probeDone=true;timer.due=UINT64_MAX;}
 void runBackgroundMonitor(){timer.due=UINT64_MAX;}void handlePowerChange(bool){}void acknowledgeSetPowerState(){}
 bool start(IOService*);bool finishStartup();void continueStartup();
 static void probeTimerFired(OSObject*,IOTimerEventSource*);
};
void publishBootCheckpoint(IOService*,const char*,IOReturn=kIOReturnSuccess){}
void publishMatchingEvent(const char*,IOService*,IOReturn){}
void retainFailedServiceEvidence(IOService*){}
#include "Service.inc"

struct Machine {
 IOACPIPlatformDevice spi,amp,gpio;IOPCIDevice pci;IOMemoryMap* pads;
 Machine(){
  nowNs=4670275000ULL;pciAtNs=10174227000ULL;powerAtNs=pciAtNs;callbackAtNs=34099898000ULL;
  callbackDone=earlyDone=missingSymbol=malformedDSD=stopped=false;
  frames=writes=csBinds=evaluations=idReads=earlyIdReads=0;bootReadyAtNs=0;resetBootDelayNs=180000000;published.clear();patcher={};
  nativePropertyEvaluator=nullptr;nativePropertySetupState="CALLBACK_NOT_REACHED";
  spi.path="IOACPIPlane:/_SB/PC00@0/SPI0@1e0002";amp.path=spi.path+"/SPK1";amp.parent=&spi;
  gpio.path="IOACPIPlane:/_SB/GPI0";gpio.values["_HID"]=make<OSString>("INTC1083");
  spiNode=&spi;ampNode=&amp;publishedPCI=&pci;::devices={&spi,&amp,&gpio};
  pci.objects["acpi-device"]=&spi;pci.objects["acpi-path"]=make<OSString>(spi.path.c_str());
  std::vector<uint8_t> crs;
  const char* hex=
#include "Resources.inc"
  ;
  for(size_t i=0;hex[i];i+=2){unsigned value=0;assert(sscanf(hex+i,"%2x",&value)==1);crs.push_back(uint8_t(value));}
  amp.values["_CRS"]=make<OSData>(crs);
  spi.values["_DSD"]=make<OSNumber>(0);spi.dsdNullResult=true;
  pads=make<IOMemoryMap>();pads->r[0]=0x960000;pads->r[3]=0x700;
  pads->r[(0x700+27*16)/4]=0x84000201;gpio.maps={{1,pads}};
  callbackAction=[](){patcher.updateRunningInfo(1,0xFFFFFF8008C01000ULL,491520);acpiLoaded(nullptr,patcher,1,0xFFFFFF8008C01000ULL,491520);};
  earlyAction=[](){runEarlyPatcher();};
 }
 ~Machine(){assert(!liveControllers&&!allocations);assert(!gpio.opened&&gpio.opens==gpio.closes);assert(pci.refs==1&&spi.refs==1);}
 void run(CirrusAudioFixup& service,uint64_t until){
  for(unsigned ticks=0;ticks<10000;++ticks){
   auto due=std::min(service.timer.due,until);replayAdvance(due);
   if(service.timer.due<=nowNs){service.timer.due=UINT64_MAX;CirrusAudioFixup::probeTimerFired(&service,&service.timer);}
   if(nowNs>=until)return;
  }
  assert(false);
 }
};

int main(){
 {Machine m;CirrusAudioFixup s;
  assert(s.start(&m.amp));assert(s.mStartupPending&&!s.finishes&&!frames&&!writes&&!csBinds);
  acpiLoaded(nullptr,patcher,13,0xFFFFFF8008C01000ULL,491520);assert(!nativePropertyEvaluator);
  m.run(s,callbackAtNs-1);assert(s.mStartupPending&&!frames&&!csBinds);
  m.run(s,callbackAtNs+200000000);assert(!s.mStartupPending&&!s.mStartupFailed&&s.finishes==1&&frames==2&&csBinds==1&&!writes);
  assert(evaluations==1&&s.probeDone);assert(m.pads->r[(0x700+27*16)/4]==0x84000201);
  s.stop(&m.amp);puts("PASS qumADC PCI 5.5s + ACPI 34.1s -> GPIO CS -> both read-only ID frames");}
 {Machine m;CirrusAudioFixup s;callbackAtNs=5000000000ULL;powerAtNs=12000000000ULL;
  assert(s.start(&m.amp));assert(s.mStartupPending&&!frames);m.run(s,powerAtNs+200000000);
  assert(s.finishes==1&&frames==2&&csBinds==1&&!writes);s.stop(&m.amp);puts("PASS callback before PCI/D0");}
 {Machine m;CirrusAudioFixup s;callbackAtNs=UINT64_MAX;
  assert(s.start(&m.amp));m.run(s,nowNs+61000000000ULL);
  assert(s.mStartupFailed&&!s.mNativeSPI.bound()&&!frames&&!writes&&s.firstFailure=="ACPI_INTERPRETER_TIMEOUT");
  s.stop(&m.amp);puts("PASS missing callback bounded failure and cleanup");}
 {Machine m;CirrusAudioFixup s;missingSymbol=true;
  assert(s.start(&m.amp));m.run(s,callbackAtNs+20000000);
  assert(s.mStartupFailed&&!frames&&!writes&&s.firstFailure=="ACPI_INTERPRETER_SETUP_FAILED");s.stop(&m.amp);puts("PASS missing symbol is terminal");}
 {Machine m;CirrusAudioFixup s;malformedDSD=true;
  assert(s.start(&m.amp));m.run(s,callbackAtNs+20000000);
  assert(s.mStartupFailed&&!frames&&!writes&&s.firstFailure=="INTEL_SPI_CHIP_SELECT_CONFIGURATION_FAILED");s.stop(&m.amp);puts("PASS malformed reference never reaches SPI");}
 {Machine m;CirrusAudioFixup s;
  assert(s.start(&m.amp));s.stop(&m.amp);m.run(s,callbackAtNs+20000000);
  assert(!frames&&!writes&&!csBinds&&!s.finishes);puts("PASS stop while ACPI pending prevents later initialization");}
 {Machine m;CirrusAudioFixup s;s.readOnly=false;
  assert(s.start(&m.amp));m.run(s,callbackAtNs+400000000);
  assert(s.mAmplifierCount==2&&!s.mStartupFailed&&!writes&&idReads==2&&!earlyIdReads);s.stop(&m.amp);puts("PASS cold reset waits for firmware boot before DEVID");}
 {Machine m;CirrusAudioFixup s;s.readOnly=false;resetBootDelayNs=1000000000;
  assert(s.start(&m.amp));m.run(s,callbackAtNs+700000000);
  assert(!s.mAmplifierCount&&!idReads&&!writes&&s.firstFailure=="SPI_IDENTITY_PREPARATION_FAILED");
  assert(s.numbers["Cirrus_SPI_0_Identity_Prepare_Return"]==uint32_t(kIOReturnTimeout));
  s.stop(&m.amp);puts("PASS firmware never boots: no DEVID or writes after deadline");}
 for(uint64_t callback:{5000000000ULL,12000000000ULL,40000000000ULL})
  for(uint64_t power:{10500000000ULL,15000000000ULL}){
   Machine m;CirrusAudioFixup s;callbackAtNs=callback;powerAtNs=power;
   assert(s.start(&m.amp));m.run(s,std::max(callbackAtNs,powerAtNs)+200000000);
   assert(s.mAmplifierCount==2&&csBinds==1&&idReads==2&&!earlyIdReads&&!writes);s.stop(&m.amp);
  }
 puts("PASS six callback/PCI/D0 ordering permutations");
 {Machine m;CirrusAudioFixup s;pciAtNs=nowNs+31000000000ULL;
  assert(!s.start(&m.amp));assert(!frames&&!writes&&!csBinds&&s.firstFailure=="PCI_ASSOCIATION_UNAVAILABLE_OR_ACPI_CONTROLLER_UNSUPPORTED");puts("PASS absent PCI bounded failure");}
}
