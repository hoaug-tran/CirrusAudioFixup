from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from host_build import sanitizer_flags
root=Path(__file__).resolve().parents[2]
source=(root/'CirrusAudioFixup/Platform/HDA/AudioEventSource.cpp').read_text()
assert 'updateRunningInfo(' not in source
assert 'onPatcherLoad(' not in source
callback=source[source.index('    void acpiLoaded('):source.index('\n#endif',source.index('    void acpiLoaded('))]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define IOLog(...) std::printf(__VA_ARGS__)
using mach_vm_address_t=uintptr_t;
struct Info {size_t loadIndex=7;} acpiInfo[1];
struct KernelPatcher {};
namespace cirrus::transport {
inline const char* nativePropertySetupState="CALLBACK_NOT_REACHED";
inline unsigned setups=0;
void configureACPIEvaluation(KernelPatcher&,size_t index,uintptr_t start,size_t size){
 assert(index==7&&start==0x1234000&&size==0x10000);++setups;nativePropertySetupState="READY";
}
}
'''
checks=r'''
int main(){
 using namespace cirrus::transport;
 KernelPatcher patcher;
 acpiLoaded(nullptr,patcher,6,0x1234000,0x10000);assert(!setups);
 acpiLoaded(nullptr,patcher,7,0x1234000,0x10000);assert(setups==1);

}
'''
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);cpp=path/'test.cpp';exe=path/'test.exe'
 cpp.write_text(fixture+callback+checks)
 for flags in sanitizer_flags():
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('PASS ACPI setup uses the matching Lilu callback image; no premature MachInfo initialization')
