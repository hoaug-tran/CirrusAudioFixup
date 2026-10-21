"""Exercise the production retained boot journal, including first-error limits."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from production_source_fixture import service_source
import subprocess
import tempfile
from acpi_kernel_fixture import install

ROOT = Path(__file__).resolve().parents[2]
source = service_source()
start = source.index("    inline void publishBootCheckpoint(")
checkpoint = source[start:source.index("    inline void retainFailedServiceEvidence(", start)]
with tempfile.TemporaryDirectory(prefix="cirrus-boot-evidence-") as directory:
    tmp = Path(directory)
    install(tmp)
    fixture = tmp / "Fixture.hpp"
    text = fixture.read_text()
    text = text.replace('strings[k]=v;return true;', 'strings[k]=v;objects[k]=make<OSString>(v);return true;')
    text = text.replace('numbers[k]=v;return true;', 'numbers[k]=v;objects[k]=make<OSNumber>(v);return true;')
    fixture.write_text(text)
    cpp = tmp / "test.cpp"
    cpp.write_text(r'''
#include "Fixture.hpp"
bool gCirrusDebug=true;
uint64_t clockNs=1000;
uint64_t mach_absolute_time(){return clockNs+=1000;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t* value){*value=t;}
''' + checkpoint + r'''
int main(){
 IOService owner;
 publishBootCheckpoint(nullptr,"NULL",kIOReturnError);
 gCirrusDebug=false;publishBootCheckpoint(&owner,"DISABLED",kIOReturnError);
 assert(owner.objects.empty());gCirrusDebug=true;
 publishBootCheckpoint(&owner,"COMMUNICATION_BEGIN");
 publishBootCheckpoint(&owner,"DOMAIN_TIMEOUT",kIOReturnTimeout);
 publishBootCheckpoint(&owner,"SHUTDOWN");
 publishBootCheckpoint(&owner,"CLEANUP_FAILURE",kIOReturnIOError);
 assert(owner.strings["Cirrus_Boot_First_Error"]=="DOMAIN_TIMEOUT");
 assert(owner.numbers["Cirrus_Boot_First_Return"]==uint32_t(kIOReturnTimeout));
 assert(owner.strings["Cirrus_Boot_Checkpoint_001"].find("DOMAIN_TIMEOUT")!=std::string::npos);
 for(unsigned i=0;i<150;++i)publishBootCheckpoint(&owner,"POLL");
 assert(owner.numbers["Cirrus_Boot_Checkpoint_Count"]==96);
 assert(!owner.strings.count("Cirrus_Boot_Checkpoint_096"));
 assert(owner.strings["Cirrus_Boot_Checkpoint_Status"]=="PARTIAL_LIMIT");
 assert(owner.strings["Cirrus_Boot_First_Error"]=="DOMAIN_TIMEOUT");
 assert(owner.strings["Cirrus_Boot_Checkpoint_Last"]=="POLL");
}
''')
    exe = tmp / "test.exe"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS retained boot evidence: first failure survives cleanup, bounded journal and disabled logging")

# Exercise deferred startup, PM acknowledgement and stop ordering.
from production_source_fixture import method
lifecycle = "\n".join(method(source, "CirrusAudioFixup", name) for name in
                      ["continueStartup", "probeTimerFired", "lifecycleAction", "setPowerState", "stop"])
cpp = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <map>
using IOReturn=int;
constexpr bool kOSBooleanTrue=true;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnBadArgument=-3,IOPMAckImplied=0;
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
#define OSDynamicCast(T,p) static_cast<T*>(p)
struct OSObject {};
struct IOService:OSObject {void stop(IOService*){}};
#define OSSafeReleaseNULL(p) do {p=nullptr;}while(0)
struct IOTimerEventSource {unsigned arms=0;void setTimeoutMS(unsigned){++arms;}void cancelTimeout(){}};
struct CirrusAudioFixup:IOService {
 using super=IOService;
 bool mPMInitialized=false;
 struct Loop{template<class T>void removeEventSource(T*){}}loop;Loop* mWorkLoop=&loop;
 unsigned unsafeTeardowns=0;
 void teardownAmplifierInterrupt(){if(!mStopping)++unsafeTeardowns;}
 void teardownAudioEvents(){if(!mStopping)++unsafeTeardowns;}
 void PMstop(){}
 void stop(IOService*);
 bool mStopping=false,mStartupFailed=false,mStartupPending=true,mResumePending=false,mPowerAvailable=true;
 bool finishOK=true,setupOK=true,wakeReady=false;
 unsigned finishes=0,setups=0,teardowns=0,failures=0,acks=0,monitors=0,probes=0;
 IOService provider;IOService* mProvider=&provider;
 IOTimerEventSource timer;IOTimerEventSource* mProbeTimer=&timer;
 struct Native {
  bool pending=true;int result=kIOReturnNotReady;unsigned polls=0;
  struct Failure{const char* reason="DOMAIN_TIMEOUT";unsigned index=0;} f;
  int continueBind(){++polls;return result;}
  bool powerPending(){return pending;}
  const Failure& failure(){return f;}
 }mNativeSPI;
 struct Gate {CirrusAudioFixup* self;int runAction(int(*f)(OSObject*,void*,void*,void*,void*),void* a){return f(self,a,nullptr,nullptr,nullptr);}} gate{this};
 Gate* mCommandGate=&gate;
 std::map<std::string,std::string> properties;
 void setProperty(const char* k,const char* v){properties[k]=v;}
 void setProperty(const char*,bool){}
 void recordAttachmentFailure(const char*,const char*,int,unsigned,const char*){++failures;}
 bool setupCommunication(IOService*){assert(!mNativeSPI.pending);++setups;return setupOK;}
 bool finishStartup(){assert(!mNativeSPI.pending);++finishes;return finishOK;}
 void teardownCommunication(){++teardowns;}
 bool bootArgEnabled(const char*){return false;}
 void runReadOnlyProbe(){++probes;}
 void runBackgroundMonitor(){++monitors;}
 void acknowledgeSetPowerState(){++acks;}
 void handlePowerChange(bool on){if(!on){mResumePending=false;mPowerAvailable=false;}else{mResumePending=!wakeReady;mPowerAvailable=wakeReady;}}
 void continueStartup();
 static void probeTimerFired(OSObject*,IOTimerEventSource*);
 static int lifecycleAction(OSObject*,void*,void*,void*,void*);
 int setPowerState(unsigned long,IOService*);
};
''' + lifecycle + r'''
int main(){
 CirrusAudioFixup start;
 for(unsigned i=0;i<80;++i)start.probeTimerFired(&start,&start.timer);
 assert(start.mStartupPending&&!start.setups&&!start.finishes&&!start.teardowns&&!start.failures&&!start.monitors);
 start.mNativeSPI.pending=false;start.mNativeSPI.result=0;
 start.probeTimerFired(&start,&start.timer);
 assert(!start.mStartupPending&&start.setups==1&&start.finishes==1&&!start.teardowns);
 start.probeTimerFired(&start,&start.timer);assert(start.monitors==1&&start.finishes==1);
 CirrusAudioFixup timeout;timeout.mNativeSPI.pending=false;timeout.mNativeSPI.result=-4;
 timeout.probeTimerFired(&timeout,&timeout.timer);
 assert(timeout.mStartupFailed&&timeout.failures==1&&timeout.teardowns==1&&!timeout.finishes);
 timeout.probeTimerFired(&timeout,&timeout.timer);assert(timeout.failures==1&&timeout.teardowns==1);
 CirrusAudioFixup asleep;asleep.mPowerAvailable=false;
 asleep.probeTimerFired(&asleep,&asleep.timer);assert(!asleep.mNativeSPI.polls&&!asleep.finishes);
 CirrusAudioFixup stopped;stopped.mStopping=true;
 stopped.probeTimerFired(&stopped,&stopped.timer);assert(!stopped.mNativeSPI.polls&&!stopped.finishes);
 CirrusAudioFixup wake;wake.mStartupPending=false;wake.mPowerAvailable=false;
 assert(wake.setPowerState(1,&wake)==35000000&&wake.mResumePending&&!wake.acks);
 for(unsigned i=0;i<80;++i)wake.probeTimerFired(&wake,&wake.timer);
 assert(wake.mResumePending&&!wake.acks&&!wake.monitors);
 wake.wakeReady=true;wake.probeTimerFired(&wake,&wake.timer);
 assert(!wake.mResumePending&&wake.acks==1&&wake.mPowerAvailable);
 wake.probeTimerFired(&wake,&wake.timer);assert(wake.acks==1&&wake.monitors==1);
 CirrusAudioFixup shutdown;shutdown.stop(shutdown.mProvider);
 assert(shutdown.mStopping&&!shutdown.unsafeTeardowns&&shutdown.teardowns==1&&!shutdown.mProvider);
 CirrusAudioFixup cancel;cancel.mStartupPending=false;cancel.mResumePending=true;cancel.mPowerAvailable=false;
 assert(cancel.lifecycleAction(&cancel,reinterpret_cast<void*>(uintptr_t(2)),nullptr,nullptr,nullptr)==0);
 cancel.probeTimerFired(&cancel,&cancel.timer);
 assert(cancel.mStopping&&!cancel.mResumePending&&cancel.acks==1&&!cancel.monitors);
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-deferred-start-") as directory:
    tmp=Path(directory);test=tmp/"test.cpp";test.write_text(cpp);exe=tmp/"test.exe"
    subprocess.run(["g++","-std=c++17","-Wall","-Wextra","-Werror",str(test),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
assert 'getProperty("IOPowerManagement")' not in (ROOT/"CirrusAudioFixup/Transport/SPI/Intel/IntelLPSSSPIController.cpp").read_text()
print("PASS deferred startup, pending wake acknowledgement, timeout and stop cancellation without premature hardware initialization")
