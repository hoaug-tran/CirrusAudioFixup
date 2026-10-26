from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'CirrusAudioFixup/Platform/HDA/AudioEventSource.cpp').read_text()
callback=source[source.index('    void acpiPatcherReady('):source.index('    void acpiLoaded(')]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define IOLog(...) std::printf(__VA_ARGS__)
struct Info {unsigned loadIndex=0;} acpiInfo[1];
struct KernelPatcher {
 enum class Error {NoError,AlreadyDone,NoKinfoFound,KernRunningInitFailure};
 Error error=Error::NoError,loadError=Error::NoError,runningError=Error::NoError;
 unsigned updates=0;
 unsigned long loadKinfo(Info* info){error=loadError;info->loadIndex=7;return 7;}
 Error getError(){return error;}
 void clearError(){error=Error::NoError;}
 void updateRunningInfo(unsigned index){assert(index==7);++updates;error=runningError;}
};
namespace cirrus::transport {
inline const char* nativePropertySetupState="CALLBACK_NOT_REACHED";
inline unsigned setups=0;
void configureACPIEvaluation(KernelPatcher&,unsigned index,uintptr_t start,size_t size){
 assert(index==7&&!start&&!size);++setups;nativePropertySetupState="READY";
}
}
'''
checks=r'''
int main(){
 using namespace cirrus::transport;
 KernelPatcher patcher;
 patcher.loadError=KernelPatcher::Error::NoKinfoFound;
 acpiPatcherReady(nullptr,patcher);assert(!setups&&!patcher.updates);
 patcher.loadError=KernelPatcher::Error::NoError;
 patcher.runningError=KernelPatcher::Error::KernRunningInitFailure;
 acpiPatcherReady(nullptr,patcher);assert(!setups&&patcher.updates==1);
 patcher.runningError=KernelPatcher::Error::NoError;
 acpiPatcherReady(nullptr,patcher);assert(setups==1);
 patcher.loadError=KernelPatcher::Error::AlreadyDone;
 acpiPatcherReady(nullptr,patcher);assert(setups==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);cpp=path/'test.cpp';exe=path/'test.exe'
 cpp.write_text(fixture+callback+checks)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS production early ACPI setup: kinfo failure, running-image failure, success without kext dispatch, existing kinfo')
