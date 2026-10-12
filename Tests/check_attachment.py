"""Attachment evidence, CPU metadata and registered debug policy boundaries."""
from pathlib import Path
import subprocess,tempfile
from driver_kernel_fixture import install
ROOT=Path(__file__).resolve().parents[1]
CPP=r'''
#include "Diagnostics/AttachmentState.hpp"
#include "Support/CPUIdentity.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include <cassert>
using namespace cirrus;
int main(){
    diagnostics::AttachmentState state;
    diagnostics::AttachmentState invalid;assert(invalid.record(nullptr,nullptr,-99,0));assert(!strcmp(invalid.first.stage,"UNKNOWN") && !strcmp(invalid.first.reason,"UNSPECIFIED_FAILURE"));
    assert(state.record("COMMUNICATION","PROVIDER_UNSUPPORTED",-77,1));
    assert(state.record("CLEANUP","RELEASE_FAILED",-88,2));
    assert(!strcmp(state.first.reason,"PROVIDER_UNSUPPORTED") && state.first.result==-77 && state.first.index==1);
    assert(!strcmp(state.latest.reason,"RELEASE_FAILED") && state.latest.result==-88 && state.latest.index==2);
    while(state.failureCount<31)assert(!state.record("CLEANUP","RELEASE_FAILED",-88,2));
    assert(state.record("CLEANUP","RELEASE_FAILED",-88,2) && state.failureCount==32);
    assert(state.record("CLEANUP","RELEASE_FAILED",-89,2));
    assert(state.record("CLEANUP","RELEASE_FAILED",-89,3));
    assert(state.first.result==-77);
    assert(state.record("CONTROLLER_PM","RESUME_FAILED",-90,1,"CONTROLLER_SLOT") && !strcmp(state.latest.indexKind,"CONTROLLER_SLOT") && !strcmp(state.first.indexKind,"ENDPOINT"));
    state.record("CLEANUP","RELEASE_FAILED",-89,3);
    state.failureCount=UINT32_MAX;assert(!state.record("CLEANUP","RELEASE_FAILED",-89,3) && state.failureCount==UINT32_MAX);
    assert(!strcmp(support::cpuVendorName("GenuineIntel"),"INTEL"));
    assert(!strcmp(support::cpuVendorName("AuthenticAMD"),"AMD"));
    assert(!strcmp(support::cpuVendorName(nullptr),"UNKNOWN") && !strcmp(support::cpuVendorName("GenuineIntelX"),"UNKNOWN"));
    const auto cpu=support::readCPUIdentity();assert(cpu.vendor[12]==0 && cpu.brand[48]==0);
    platform::AmplifierDriverRegistry registry;
    core::RegisterMapConfig format;format.readable=[](uint32_t){return true;};format.writable=format.readable;
    platform::AmplifierDriverRegistration definition{"phase-fixture","AMPFIXTURE",0x1111,1,format,{},
        [](const platform::AmplifierDriverContext&)->core::AmplifierLifecycleHooks*{return nullptr;},[](core::AmplifierLifecycleHooks*){}};
    definition.validDebugPhase=[](const char* phase){return !strcmp(phase,"probe") || !strcmp(phase,"firmware");};
    assert(registry.add(definition));
    assert(!registry.acceptsDebugPhase(nullptr,"probe"));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE",nullptr));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE","") && !registry.acceptsDebugPhase("UNKNOWN","probe"));
    assert(registry.acceptsDebugPhase("AMPFIXTURE","probe") && registry.acceptsDebugPhase("AMPFIXTURE","firmware"));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE","probe-extra"));
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-attachment-') as directory:
    p=Path(directory);install(p);(p/'attachment.cpp').write_text(CPP)
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['clang++','-std=c++17',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'attachment.cpp'),'-o',str(p/'attachment')],check=True)
        subprocess.run([str(p/'attachment')],check=True)
print('PASS attach first/latest native errors, rate limits/saturation, bounded CPU identity and null/unknown debug phase policy')

# Execute the production dependency retry, including complete startup ordering.
from production_source_fixture import method
service=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text()
retry=method(service,'CirrusAudioFixup','retryPlatformIdentity')
cpp=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <map>
using IOReturn=int;
constexpr int kIOReturnTimeout=-1,kIOReturnNotPermitted=-2;
uint64_t now=0;uint64_t playbackTimeMilliseconds(){return now;}
void IOSleep(unsigned){}
#define CIRRUS_LOG(...) do{}while(0)
struct Timer{unsigned arms=0;void setTimeoutMS(unsigned){++arms;}};
class CirrusAudioFixup{
public:
 bool mPlatformIdentityPending=true,mPlatformAuthorized=false;uint64_t mPlatformIdentityDeadlineMs=30000;
 uint32_t mPlatformSubsystemId=0,nextIdentity=0;void* mProvider=nullptr;
 Timer timer;Timer* mProbeTimer=&timer;
 struct Attachment{const char* amplifier="WAITING";}mAttachment;
 struct Native{bool active=false;bool bound(){return active;}}mNativeSPI;
 bool setupOK=true,wrongSilicon=false;unsigned resets=0,detected=0,initialized=0,teardowns=0,irq=0,failures=0,verdicts=0;
 std::string order,reason;std::map<std::string,std::string> properties;
 void resolvePlatformProfile(){mPlatformSubsystemId=nextIdentity;mPlatformAuthorized=nextIdentity==0x17aa382b;}
 void setProperty(const char* k,const char* v){properties[k]=v;}
 void recordAttachmentFailure(const char*,const char* r,int){++failures;reason=r;}
 void publishDriverVerdict(){++verdicts;}
 void teardownCommunication(){++teardowns;order+='T';}
 bool setupCommunication(void*){order+='C';if(wrongSilicon)mPlatformAuthorized=false;return setupOK;}
 bool performPlatformHardwareReset(){assert(mPlatformAuthorized);++resets;order+='R';return true;}
 void detectNativeSPIAmplifiers(){++detected;order+='D';}
 void detectAmplifiers(){++detected;order+='D';}
 void bringUpAmplifiers(){++initialized;order+='I';}
 void setupAmplifierInterrupt(){++irq;}
 bool retryPlatformIdentity();
};
'''
checks=r'''
int main(){
 CirrusAudioFixup late;
 for(now=0;now<21000;now+=100){assert(late.retryPlatformIdentity());assert(!late.resets&&!late.initialized&&!late.failures);}
 late.nextIdentity=0x17aa382b;assert(late.retryPlatformIdentity());
 assert(!late.mPlatformIdentityPending && late.order=="TCRDI" && late.resets==1 && late.initialized==1);
 assert(!late.retryPlatformIdentity() && late.resets==1);
 CirrusAudioFixup unsupported;unsupported.nextIdentity=0x12345678;
 assert(unsupported.retryPlatformIdentity()&&!unsupported.resets&&!unsupported.teardowns&&unsupported.reason=="BOARD_NOT_AUTHORIZED");
 CirrusAudioFixup timeout;now=30000;assert(timeout.retryPlatformIdentity()&&timeout.reason=="HDA_PCI_IDENTITY_TIMEOUT"&&!timeout.resets&&timeout.timer.arms==0);
 assert(!timeout.retryPlatformIdentity());
 CirrusAudioFixup rejected;rejected.nextIdentity=0x17aa382b;rejected.setupOK=false;
 assert(rejected.retryPlatformIdentity()&&!rejected.resets&&!rejected.initialized);
 CirrusAudioFixup wrong;wrong.nextIdentity=0x17aa382b;wrong.wrongSilicon=true;
 assert(wrong.retryPlatformIdentity()&&!wrong.resets&&!wrong.initialized);
 CirrusAudioFixup spi;spi.nextIdentity=0x17aa382b;spi.mNativeSPI.active=true;assert(spi.retryPlatformIdentity()&&spi.detected==1&&spi.initialized==1);
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-board-wait-') as directory:
    p=Path(directory);(p/'wait.cpp').write_text(cpp+retry+checks)
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['clang++','-std=c++17',*flags,str(p/'wait.cpp'),'-o',str(p/'wait')],check=True)
        subprocess.run([str(p/'wait')],check=True)
assert 'Author: Tran Kinh Hoang (hoaugtr)' in service
assert service.index('mPlatformIdentityPending = !mPlatformSubsystemId') < service.index('bool hardwareResetVerified = performPlatformHardwareReset()')
print('PASS late board identity at 21s, bounded timeout, unknown board, preflight failure and no reset/init before verified authorization')
