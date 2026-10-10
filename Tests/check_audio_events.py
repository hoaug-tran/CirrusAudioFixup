"""Exercise the production hook and mailbox code with a small IOKit fixture."""
from pathlib import Path
import re
import plistlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / 'CirrusAudioFixup/Platform/HDA/AudioEvents.hpp').read_text(encoding='utf-8')
source = (ROOT / 'CirrusAudioFixup/Platform/HDA/AudioEvents.cpp').read_text(encoding='utf-8')
sdk_types = (ROOT / 'MacKernelSDK/Headers/IOKit/audio/IOAudioTypes.h').read_text(encoding='utf-8')
engine_enum = re.search(r'typedef\s+enum\s+(\w+)\s*\{[^}]*kIOAudioEngineRunning', sdk_types, re.S).group(1)
assert f'__ZN13IOAudioEngine8setStateE{len(engine_enum)}{engine_enum}' in source, 'Hook must use the SDK enum tag, not its typedef alias'
strip_includes = lambda text: re.sub(r'^\s*#(?:include|pragma).*$', '', text, flags=re.M)
metadata = plistlib.loads((ROOT / 'CirrusAudioFixup/Info.plist').read_bytes())
bootstrap = metadata['IOKitPersonalities']['CirrusAudioBootstrap']
assert bootstrap['IOProviderClass'] == 'IOResources' and bootstrap['IOResourceMatch'] == 'IOKit'
assert metadata['OSBundleRequired'] == 'Root'

fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <mutex>
using UInt32 = uint32_t;
using IOReturn = int;
using kern_return_t = int;
using mach_vm_address_t = uintptr_t;
struct kmod_info_t {};
constexpr int kIOReturnSuccess=0, KERN_SUCCESS=0, KERN_FAILURE=1;
constexpr int gIOServicePlane=0, kIORegistryIterateRecursively=1;
struct OSObject {
    int refs=1;
    virtual ~OSObject()=default;
    virtual void free() {}
    void retain() { ++refs; }
    void release() { --refs; assert(refs>=0); }
};
struct OSNumber : OSObject {
    uint32_t value;
    explicit OSNumber(uint32_t n):value(n) {}
    uint32_t unsigned32BitValue() { return value; }
};
struct IORegistryEntry : OSObject {
    IORegistryEntry* parent=nullptr;
    std::vector<IORegistryEntry*> children;
    std::map<std::string,OSObject*> properties;
    const char* kind="IOService";
    OSObject* getProperty(const char* key) { return properties[key]; }
    OSObject* copyProperty(const char* key) { auto* value=getProperty(key); if(value) value->retain(); return value; }
    IORegistryEntry* copyParentEntry(int) { if(parent) parent->retain(); return parent; }
    void* metaCast(const char* type) { return strcmp(type,kind)==0 ? this : nullptr; }
};
struct IOService : IORegistryEntry { virtual bool start(IOService*) { return true; } };
struct IOLock { std::mutex mutex; };
IOLock* IOLockAlloc() { return new IOLock; }
void IOLockFree(IOLock* lock) { delete lock; }
void IOLockLock(IOLock* lock) { lock->mutex.lock(); }
void IOLockUnlock(IOLock* lock) { lock->mutex.unlock(); }
struct IOEventSource : OSObject {
    OSObject* owner=nullptr;
    unsigned signals=0;
    bool init(OSObject* target) { owner=target; return true; }
    virtual bool checkForWork() { return false; }
    void signalWorkAvailable() { ++signals; }
};
struct IORegistryIterator : OSObject {
    std::vector<IORegistryEntry*> entries;
    size_t index=0;
    static void collect(IORegistryEntry* root,std::vector<IORegistryEntry*>& out) {
        for(auto* child:root->children) { out.push_back(child); collect(child,out); }
    }
    static IORegistryIterator* iterateOver(IORegistryEntry* root,int,int) {
        auto* it=new IORegistryIterator; collect(root,it->entries); return it;
    }
    OSObject* getNextObject() { return index<entries.size()?entries[index++]:nullptr; }
};
#define OSDynamicCast(T, p) dynamic_cast<T*>(p)
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,B)
#define OSSafeReleaseNULL(p) do { if(p) (p)->release(); (p)=nullptr; } while(0)
#define ADDPR(a) CirrusAudioFixup_##a
#define IOLog(...) ((void)0)
bool checkKernelArgument(const char*) { return false; }
int updateResult=0;
IOReturn baseUpdate(IOService* sender,OSObject* value) {
    if(!updateResult) sender->properties["IOAudioControlValue"]=value;
    return updateResult;
}
uint32_t baseState(IOService* sender,uint32_t state) {
    auto* old=dynamic_cast<OSNumber*>(sender->getProperty("IOAudioEngineState"));
    uint32_t previous=old?old->value:0;
    sender->properties["IOAudioEngineState"]=new OSNumber(state);
    return previous;
}
struct KernelPatcher {
    struct KextInfo { const char* id; const char** paths; size_t count; bool sys[6]; bool user[2]; size_t loadIndex; };
    struct RouteRequest {
        const char* symbol; mach_vm_address_t* original;
        template<class T> RouteRequest(const char* s,T,mach_vm_address_t& o):symbol(s),original(&o) {}
    };
    bool routeMultiple(size_t,RouteRequest* r,size_t,mach_vm_address_t,size_t) {
        *r->original=strstr(r->symbol,"updateValue")?reinterpret_cast<uintptr_t>(baseUpdate):reinterpret_cast<uintptr_t>(baseState);
        return true;
    }
    void clearError() {}
};
struct LiluAPI {
    enum class Error { NoError };
    Error requestAccess() { return Error::NoError; }
    void releaseAccess() {}
    Error onKextLoad(KernelPatcher::KextInfo* info,size_t,
                    void (*callback)(void*,KernelPatcher&,size_t,mach_vm_address_t,size_t)) {
        KernelPatcher patcher; info[0].loadIndex=3; callback(nullptr,patcher,3,0,0); return Error::NoError;
    }
} lilu;
'''

checks = r'''
using namespace cirrus::platform::hda;
AudioEventState delivered;
unsigned deliveries=0;
void receive(OSObject*,const AudioEventState& state) { delivered=state; ++deliveries; }
void child(IOService& object,IOService& parent,const char* kind) {
    object.parent=&parent; object.kind=kind; parent.children.push_back(&object);
}
void selector(IOService& control,uint32_t subtype,uint32_t value) {
    control.properties["IOAudioControlType"]=new OSNumber(0x736C6374);
    control.properties["IOAudioControlSubType"]=new OSNumber(subtype);
    control.properties["IOAudioControlUsage"]=new OSNumber(subtype);
    control.properties["IOAudioControlValue"]=new OSNumber(value);
}
int main() {
    assert(CirrusAudioFixup_start(nullptr,nullptr)==KERN_SUCCESS);
    assert(AudioEventSource::hooksReady());
    IOService root,engine,control,input,otherEngine,otherControl,externalRoot,externalEngine,externalControl;
    child(engine,root,"IOAudioEngine"); child(control,engine,"IOAudioSelectorControl");
    child(input,engine,"IOAudioSelectorControl");
    child(otherEngine,root,"IOAudioEngine"); child(otherControl,otherEngine,"IOAudioSelectorControl");
    child(externalEngine,externalRoot,"IOAudioEngine"); child(externalControl,externalEngine,"IOAudioSelectorControl");
    selector(control,0x6F757470,0x6973706B);
    selector(input,0x696E7074,0x6973706B);
    selector(otherControl,0x6F757470,0x6864706E);
    selector(externalControl,0x6F757470,0x6864706E);
    baseState(&engine,1);
    auto* source=AudioEventSource::create(&root,&root,receive);
    assert(source && source->attach()); source->checkForWork();
    assert(source->attach());
    assert(deliveries==1 && delivered.speakers && delivered.engineRunning);
    OSNumber headphones(0x6864706E),speakers(0x6973706B);
    updateValue(&input,&headphones); updateValue(&externalControl,&headphones);
    updateValue(&otherControl,&headphones); setState(&otherEngine,0);
    source->checkForWork(); assert(deliveries==1);
    updateResult=-1; updateValue(&control,&headphones); updateResult=0;
    source->checkForWork(); assert(deliveries==1);
    updateValue(&control,&headphones); updateValue(&control,&speakers); updateValue(&control,&headphones);
    assert(!source->unchanged(delivered.generation));
    source->checkForWork(); assert(deliveries==2 && !delivered.speakers);
    const auto generation=delivered.generation;
    source->observe(&control,true,0x6973706B,true);
    source->checkForWork();
    assert(deliveries==2 && !source->snapshot().speakers && source->unchanged(generation));
    updateValue(&control,&headphones); source->checkForWork();
    assert(deliveries==2 && source->unchanged(generation));
    assert(setState(&engine,0)==1); source->checkForWork();
    assert(deliveries==3 && !delivered.engineRunning);
    setState(&engine,1); updateValue(&control,&speakers); source->checkForWork();
    assert(deliveries==4 && delivered.engineRunning && delivered.speakers);
    updateValue(&control,&headphones); source->detach(); source->checkForWork();
    assert(deliveries==4);
    updateValue(&control,&speakers); source->checkForWork(); assert(deliveries==4);
    source->free(); delete source;
    assert(root.refs==1 && engine.refs==1);
    assert(CirrusAudioFixup_stop(nullptr,nullptr)==KERN_FAILURE);
    puts("PASS production audio hooks: original calls, controller/engine/input filtering, seed, coalescing, generation and detach");
}
'''

with tempfile.TemporaryDirectory(prefix='cirrus-audio-events-') as directory:
    tmp = Path(directory)
    cpp = tmp / 'events.cpp'
    cpp.write_text(fixture + strip_includes(header) + strip_includes(source) + checks, encoding='utf-8')
    exe = tmp / 'events.exe'
    subprocess.run(['g++', '-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
