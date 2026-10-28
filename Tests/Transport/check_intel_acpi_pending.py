"""Drive the production owner across late ACPI publication and terminal failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "CirrusAudioFixup/Transport/SPI/Intel/IntelSPIControllerOwner.hpp").read_text()
source = source[source.index("class IntelSPIControllerOwner final"):source.index("inline NativeSPIControllerOwner*")]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define KERNEL 1
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnTimeout=-2,kIOReturnUnsupported=-3;
bool gCirrusDebug=true;
uint64_t now=1;
uint64_t mach_absolute_time(){return now;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t* out){*out=t;}
struct IOService {void setProperty(const char*,const char*){} void setProperty(const char*,uint64_t,unsigned){}};
struct IOACPIPlatformDevice:IOService {};
struct IOPCIDevice {uint16_t configRead16(unsigned){return 0;}};
struct SPIController {};
struct NativeSPIControllerFailure {const char* reason="NONE";IOReturn result=0;unsigned detail=0;};
struct NativeSPIControllerOwner {
virtual ~NativeSPIControllerOwner()=default;
virtual bool powerPending()const=0;
virtual IOReturn continueBind(NativeSPIControllerFailure&)=0;
virtual IOReturn prepareGPIOGroup(IOACPIPlatformDevice*,const char*)=0;
virtual IOReturn writeGPIOGroup(bool)=0;
virtual IOReturn readGPIO(IOACPIPlatformDevice*,const char*,size_t,uint32_t&)=0;
virtual SPIController& controller()=0;virtual void suspend()=0;
virtual IOReturn resume(NativeSPIControllerFailure&)=0;
};
bool pciPending=false;unsigned csBinds=0;int csResult=0;
struct IntelLPSSSPIController:SPIController {
IOReturn bind(IOPCIDevice*,IOService*){return 0;}
bool powerPending()const{return pciPending;}
IOReturn continueBind(){return pciPending?kIOReturnNotReady:0;}
const char* bindFailureReason()const{return "POWER";}
uint32_t initialCapabilities()const{return 0;}uint32_t initialStatus()const{return 0;}
uint32_t initialClock()const{return 0;}uint32_t preparedClock()const{return 0;}uint32_t initialClockGate()const{return 0;}
IOReturn configureChipSelects(IOACPIPlatformDevice*,IOService*){++csBinds;return csResult;}
IOReturn prepareGPIOGroup(IOACPIPlatformDevice*,const char*){return 0;}
IOReturn writeGPIOGroup(bool){return 0;}
IOReturn readGPIO(IOACPIPlatformDevice*,const char*,size_t,uint32_t&){return 0;}
void suspend(){} IOReturn resume(){return 0;}
};
bool nativePropertiesReady=false;
struct ACPIDeviceProperties {
IOReturn load(IOACPIPlatformDevice*,IOService*){return nativePropertiesReady?0:kIOReturnNotReady;}
const char* reason(){return "DSD_INTERPRETER_UNAVAILABLE";}
};
using Evaluator=IOReturn(*)(IOACPIPlatformDevice*,void**);
Evaluator nativePropertyEvaluator=nullptr;
const char* nativePropertySetupState="CALLBACK_NOT_REACHED";
'''
checks = r'''
int main(){
 IOACPIPlatformDevice target;IOService service;NativeSPIControllerFailure failure;
 IntelSPIControllerOwner owner;owner.setContext(&target,&service);
 pciPending=true;assert(owner.continueBind(failure)==kIOReturnNotReady&&owner.powerPending()&&!csBinds);
 pciPending=false;assert(owner.continueBind(failure)==kIOReturnNotReady&&owner.powerPending()&&!csBinds);
 now+=34000000000ULL;assert(owner.continueBind(failure)==kIOReturnNotReady&&owner.powerPending()&&!csBinds);
 nativePropertySetupState="READY";
 assert(owner.continueBind(failure)==kIOReturnNotReady&&owner.powerPending()&&!csBinds);
 nativePropertyEvaluator=[](IOACPIPlatformDevice*,void**){return 0;};
 assert(owner.continueBind(failure)==0&&!owner.powerPending()&&csBinds==1);
 assert(owner.continueBind(failure)==0&&csBinds==1);
 nativePropertyEvaluator=nullptr;nativePropertySetupState="CALLBACK_NOT_REACHED";
 IntelSPIControllerOwner timeout;timeout.setContext(&target,&service);
 assert(timeout.continueBind(failure)==kIOReturnNotReady&&timeout.powerPending());
 now+=60000000000ULL;
 assert(timeout.continueBind(failure)==kIOReturnTimeout&&!timeout.powerPending()&&csBinds==1);
 IntelSPIControllerOwner missing;missing.setContext(&target,&service);
 assert(missing.continueBind(failure)==kIOReturnNotReady&&missing.powerPending());
 nativePropertySetupState="GET_HANDLE_SYMBOL_MISSING";
 assert(missing.continueBind(failure)==kIOReturnUnsupported&&!missing.powerPending()&&csBinds==1);
 nativePropertySetupState="CALLBACK_REGISTRATION_FAILED";
 assert(missing.continueBind(failure)==kIOReturnUnsupported&&!missing.powerPending());
 nativePropertiesReady=true;
 IntelSPIControllerOwner native;native.setContext(&target,&service);
 assert(native.continueBind(failure)==0&&!native.powerPending()&&csBinds==2);
 nativePropertiesReady=false;
 nativePropertyEvaluator=[](IOACPIPlatformDevice*,void**){return 0;};
 IntelSPIControllerOwner malformed;malformed.setContext(&target,&service);csResult=kIOReturnUnsupported;
 assert(malformed.continueBind(failure)==kIOReturnUnsupported&&!malformed.powerPending());
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-acpi-pending-") as directory:
    path = Path(directory)
    cpp = path / "test.cpp"
    cpp.write_text(fixture + source + checks)
    exe = path / "test.exe"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS owner: late ACPI, PCI pending, exactly-once CS bind, deadline, symbol/registration failure, malformed CS")
