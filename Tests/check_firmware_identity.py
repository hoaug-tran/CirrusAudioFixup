from production_source_fixture import source as production_source, chip_state
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = production_source()
start = source.index('    uint32_t codecSSID = 0;')
end = source.index('    uint32_t ssid = 0;', start)
lookup = source[start:end]
program = r'''#include <cstdint>
#include <cassert>
#include <map>
#include <string>
#include <vector>
struct OSObject { virtual ~OSObject()=default; };
struct OSNumber : OSObject { uint32_t n; explicit OSNumber(uint32_t x):n(x){} uint32_t unsigned32BitValue(){return n;} };
struct OSData : OSObject { uint32_t n; explicit OSData(uint32_t x):n(x){} unsigned getLength(){return 4;} const void* getBytesNoCopy(){return &n;} };
struct IORegistryEntry : OSObject {
    std::map<std::string,OSObject*> properties;
    IORegistryEntry* parent=nullptr;
    OSObject* getProperty(const char* key){return properties[key];}
    IORegistryEntry* getParentEntry(const char*){return parent;}
};
struct OSDictionary { std::string key; void release(){delete this;} };
struct OSIterator { std::vector<IORegistryEntry*> entries; unsigned next=0;
    OSObject* getNextObject(){return next<entries.size()?entries[next++]:nullptr;}
    void release(){delete this;}
};
#define OSDynamicCast(type,object) dynamic_cast<type*>(object)
const char* gIOServicePlane="IOService";
std::map<std::string,std::vector<IORegistryEntry*>> registry;
OSDictionary* serviceMatching(const char* key){return new OSDictionary{key};}
OSIterator* getMatchingServices(OSDictionary* match){return new OSIterator{registry[match->key]};}
uint32_t discover(){
''' + lookup + r'''return codecSSID;
}
int main(){
    assert(discover()==0);
    IORegistryEntry realtek,function,gpu,gpuFunction,zero;
    OSNumber realtekVendor(0x10EC0287),gpuVendor(0x1002AA01),ssid(0x17AA3847),wrong(0x12345678),empty(0);
    realtek.properties["IOHDACodecVendorID"]=&realtekVendor;
    function.parent=&realtek;
    function.properties["IOHDACodecFunctionSubsystemID"]=&ssid;
    gpu.properties["IOHDACodecVendorID"]=&gpuVendor;
    gpuFunction.parent=&gpu;
    gpuFunction.properties["IOHDACodecFunctionSubsystemID"]=&wrong;
    zero.parent=&realtek;
    zero.properties["IOHDACodecFunctionSubsystemID"]=&empty;
    registry["IOHDACodecFunction"]={&gpuFunction,&zero,&function};
    assert(discover()==0x17AA3847);
    OSData data(0x17AA382B);
    function.properties.clear();
    function.properties["IOHDACodecFunctionSubsystemID"]=&data;
    assert(discover()==0x17AA382B);
    registry.clear();
    function.properties.clear();
    function.properties["SubsystemID"]=&ssid;
    registry["AppleHDACodecGeneric"]={&function};
    assert(discover()==0x17AA3847);
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-identity-') as directory:
    path = Path(directory)
    (path / 'identity.cpp').write_text(program)
    subprocess.run(['g++', '-std=c++17', str(path / 'identity.cpp'), '-o', str(path / 'identity')], check=True)
    subprocess.run([str(path / 'identity')], check=True)
print('PASS production codec identity lookup: delayed publication, FunctionSubsystemID number/data, zero skip, GPU rejection and legacy property')
