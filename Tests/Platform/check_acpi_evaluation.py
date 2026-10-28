"""Exercise the production interpreter bridge and bounded reference conversion."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tests/Fixtures"))
from acpi_kernel_fixture import install
from host_build import sanitizer_flags

with tempfile.TemporaryDirectory(prefix="cirrus-acpi-evaluation-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "Headers").mkdir()
    (tmp / "Headers/kern_patcher.hpp").write_text('#pragma once\n')
    (tmp / "test.cpp").write_text(r'''
#define KERNEL 1
#include "Fixture.hpp"
#include <cstdlib>
unsigned allocations=0, evaluations=0, names=0, missingSymbol=0;
bool allocationFailure=false;
std::string activePath="\\_SB.PC00.SPI0.SPK1";
void* IOMalloc(size_t bytes){if(allocationFailure)return nullptr;++allocations;return calloc(1,bytes);}
void IOFree(void* pointer,size_t){assert(allocations);--allocations;free(pointer);}
using mach_vm_address_t=uintptr_t;
class KernelPatcher {public:uintptr_t symbol(const char*);template<class T>T solveSymbol(size_t,const char* name,uintptr_t,size_t){return reinterpret_cast<T>(symbol(name));}void clearError(){}};
#include "Platform/ACPI/ACPIEvaluation.cpp"
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
using namespace cirrus::transport;
using namespace cirrus::transport::acpi;
struct Payload {Object root,items[2],property,pair[2],values[4];char key[32];uint8_t uuid[16];};
unsigned fault=0;
uint32_t getHandleMock(void* parent,const char* path,void** result){
 assert(!parent&&std::string(path)==activePath);
 if(fault==1)return 5;*result=reinterpret_cast<void*>(0x1234);return 0;
}
uint32_t getNameMock(void* handle,uint32_t type,Buffer* result){
 ++names;assert(handle==reinterpret_cast<void*>(0x1234)&&type==0);
 if(fault==2)return 5;
 const char* path=activePath.c_str();size_t bytes=strlen(path)+1;
 assert(result->length>=bytes);memcpy(result->pointer,path,bytes);result->length=bytes;
 if(fault==15)result->length=0;
 if(fault==16)result->length=257;
 if(fault==17)result->pointer=reinterpret_cast<void*>(1);
 if(fault==18){memset(result->pointer,'A',256);result->length=256;}
 if(fault==19)static_cast<char*>(result->pointer)[0]='?';
 return 0;
}
uint32_t evaluateMock(void* handle,const char* name,const void* args,Buffer* result){
 ++evaluations;assert(handle==reinterpret_cast<void*>(0x1234)&&!strcmp(name,"_DSD")&&!args);
 if(fault==3)return 5;
 auto* p=static_cast<Payload*>(result->pointer);assert(result->length>=sizeof(*p));*p={};
 p->root.package={4,2,p->items};
 const uint8_t uuid[]={0x14,0xd8,0xff,0xda,0xba,0x6e,0x8c,0x4d,0x8a,0x91,0xbc,0x9b,0xbf,0x4a,0xa3,0x01};
 memcpy(p->uuid,uuid,16);strcpy(p->key,"reset-gpios");
 p->items[0].buffer={3,16,p->uuid};p->items[1].package={4,1,&p->property};
 p->property.package={4,2,p->pair};p->pair[0].string={2,uint32_t(strlen(p->key)),p->key};
 p->pair[1].package={4,4,p->values};p->values[0].reference={20,6,handle};
 for(unsigned i=1;i<4;++i)p->values[i].integer={1,i==1?uint64_t(1):uint64_t(0)};
 switch(fault){
 case 4:p->values[0].reference.actualType=1;break;
 case 5:p->values[0].reference.handle=nullptr;break;
 case 6:p->items[1].package.count=UINT32_MAX;break;
 case 7:p->pair[0].string.pointer=reinterpret_cast<char*>(1);break;
 case 8:p->pair[0].string.length=UINT32_MAX;break;
 case 9:p->pair[1].package.elements=&p->pair[1];p->pair[1].package.count=1;break;
 case 10:p->values[0].type=99;break;
 case 11:result->length=65537;return 0;
 case 12:p->items[0].buffer.pointer=reinterpret_cast<uint8_t*>(1);break;
 case 13:p->pair[0].string.length=3;break;
 case 14:p->pair[1].package.elements=reinterpret_cast<Object*>(reinterpret_cast<uint8_t*>(p)+1);break;
 case 20:result->pointer=reinterpret_cast<void*>(1);return 0;
 case 21:result->length=sizeof(Object)-1;return 0;
 }
 result->length=sizeof(*p);return 0;
}
uintptr_t KernelPatcher::symbol(const char* symbol){
 if(!strcmp(symbol,"_AcpiGetHandle"))return missingSymbol==1?0:reinterpret_cast<uintptr_t>(getHandleMock);
 if(!strcmp(symbol,"_AcpiGetName"))return missingSymbol==2?0:reinterpret_cast<uintptr_t>(getNameMock);
 if(!strcmp(symbol,"_AcpiEvaluateObject"))return missingSymbol==3?0:reinterpret_cast<uintptr_t>(evaluateMock);
 assert(false);return 0;
}
int main(){
 IOACPIPlatformDevice amp;amp.path="IOACPIPlane:/_SB/PC00/SPI0/SPK1";
 amp.values["_DSD"]=make<OSNumber>(0);amp.dsdNullResult=true;
 OSObject* result=nullptr;KernelPatcher patcher;
 assert(evaluateNativeProperties(&amp,&result)==kIOReturnNotReady);
 for(unsigned attempt=0;attempt<3;++attempt){
  ACPIDeviceProperties properties;
  assert(properties.load(&amp)==kIOReturnNotReady);
  assert(!strcmp(properties.reason(),"DSD_INTERPRETER_UNAVAILABLE"));
  assert(!properties.object()&&!properties.get("reset-gpios"));
  assert(!evaluations&&!allocations);
 }
 assert(amp.strings["Cirrus_ACPI_Interpreter_Setup"]=="CALLBACK_NOT_REACHED");
 for(missingSymbol=1;missingSymbol<=3;++missingSymbol){
  configureACPIEvaluation(patcher,0,0,0);assert(!nativePropertyEvaluator);
  const char* reasons[]={"GET_HANDLE_SYMBOL_MISSING","GET_NAME_SYMBOL_MISSING","EVALUATE_OBJECT_SYMBOL_MISSING"};
  ACPIDeviceProperties properties;assert(properties.load(&amp)==kIOReturnNotReady);
  assert(amp.strings["Cirrus_ACPI_Interpreter_Setup"]==reasons[missingSymbol-1]);
  assert(!evaluations&&!allocations);
 }
 assert(amp.strings["Cirrus_ACPI_Interpreter_State"]=="UNAVAILABLE");
  missingSymbol=0;configureACPIEvaluation(patcher,0,0,0);assert(nativePropertyEvaluator);
 {
  ACPIDeviceProperties properties;assert(properties.load(&amp)==0);
  assert(!strcmp(properties.source(),"_DSD_INTERPRETER"));
  auto* values=OSDynamicCast(OSArray,properties.get("reset-gpios"));assert(values&&values->getCount()==4);
  assert(OSDynamicCast(OSString,values->getObject(0))->isEqualTo("\\_SB.PC00.SPI0.SPK1"));
  assert(OSDynamicCast(OSNumber,values->getObject(1))->unsigned64BitValue()==1);
  assert(properties.load(&amp)==kIOReturnBadArgument);
 }
 assert(!allocations&&evaluations==1&&names==1);
  assert(amp.strings["Cirrus_ACPI_Interpreter_State"]=="READY");
  assert(amp.numbers["Cirrus_ACPI_Evaluate_Status"]==0);
  fault=3;{ACPIDeviceProperties properties;assert(properties.load(&amp)==kIOReturnIOError);}
  assert(amp.numbers["Cirrus_ACPI_Evaluate_Status"]==5);
 for(fault=1;fault<=21;++fault){ACPIDeviceProperties properties;assert(properties.load(&amp)!=0);assert(!properties.get("reset-gpios")&&!allocations);}
 fault=2;{ACPIDeviceProperties properties;assert(properties.load(&amp)!=0);}
 assert(amp.strings["Cirrus_ACPI_Conversion_Reason"]=="REFERENCE_NAME_LOOKUP_FAILED");
 assert(amp.numbers["Cirrus_ACPI_Reference_Name_Status"]==5);
 fault=4;{ACPIDeviceProperties properties;assert(properties.load(&amp)!=0);}
 assert(amp.strings["Cirrus_ACPI_Conversion_Reason"]=="REFERENCE_TYPE_UNSUPPORTED");
 fault=0;
 activePath="\\_SB.PCI0.SPI8.AMPS";amp.path="IOACPIPlane:/_SB/PCI0/SPI8/AMPS";
 {ACPIDeviceProperties properties;assert(properties.load(&amp)==0);}
 Object dataNode{};dataNode.reference={20,4,reinterpret_cast<void*>(0x1234)};
 ObjectConverter dataConverter(&dataNode,sizeof(dataNode),getNameMock);
 auto* converted=dataConverter.copy(&dataNode);assert(OSDynamicCast(OSString,converted));converted->release();
 allocationFailure=true;{ACPIDeviceProperties properties;assert(properties.load(&amp)==kIOReturnNoMemory);}
 allocationFailure=false;
 amp.dsdNullResult=false;unsigned before=evaluations;
 {ACPIDeviceProperties properties;assert(properties.load(&amp)!=0);}assert(evaluations==before);
 amp.dsdEvaluation=kIOReturnIOError;
 {ACPIDeviceProperties properties;assert(properties.load(&amp)==kIOReturnIOError);}assert(evaluations==before);
 amp.dsdEvaluation=0;amp.values.erase("_DSD");
 amp.values["CSDP"]=make<OSNumber>(1);
 {ACPIDeviceProperties properties;assert(properties.load(&amp)==0&&!properties.get("reset-gpios"));}
 assert(evaluations==before&&!allocations);
}
''')
    executable = tmp / "test.exe"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
               "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"), "-I" + str(ROOT / "Lilu/Lilu"),
               str(tmp / "test.cpp"), "-o", str(executable)]
    for flags in sanitizer_flags():
        subprocess.run(command + flags, check=True)
        subprocess.run([str(executable)], check=True)
print("PASS native ACPI references, symbol availability, owned conversion, bounded malformed results and failure cleanup")
