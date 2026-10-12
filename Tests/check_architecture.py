from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text(encoding="utf-8")

cpp = r'''
#include <cassert>
#include <cstring>
#include "Core/AmplifierDescriptors.hpp"
#include "Platform/PlatformProfile.hpp"

int main() {
    using namespace cirrus;

    const auto* cs35l41 = core::findAmplifierById(0x00035A40);
    assert(cs35l41);
    assert(cs35l41->model == core::AmplifierModel::CS35L41);
    assert(cs35l41->hasHaloDsp);
    assert(core::findAmplifierById(0x00000000) == nullptr);
    assert(core::supportsAmplifierModel(core::AmplifierModel::CS35L41));
    assert(!core::supportsAmplifierModel(core::AmplifierModel::CS35L56));

    const auto* lenovo = platform::findPlatformProfile("CLSA0100");
    assert(lenovo);
    assert(lenovo->amplifierModel == core::AmplifierModel::CS35L41);
    assert(lenovo->endpointCount == 2);
    assert(lenovo->endpoints[0].address == 0x40);
    assert(lenovo->endpoints[1].address == 0x41);
    assert(lenovo->resetQuirk);
    assert(lenovo->allowAutomaticInitialization);
    assert(platform::permitsHardwareInitialization(*lenovo,0x17AA382B));
    assert(platform::permitsHardwareInitialization(*lenovo,0x17AA3847));
    assert(!platform::permitsHardwareInitialization(*lenovo,0));
    assert(!platform::permitsHardwareInitialization(*lenovo,0x103C89C6));
    assert(std::strcmp(lenovo->resetQuirk->controllerName, "AMDI0030") == 0);

    const auto* generic = platform::findPlatformProfile("CSC3551");
    assert(generic);
    assert(generic->resetQuirk == nullptr);
    assert(!generic->allowAutomaticInitialization);
    assert(platform::findPlatformProfile("UNKNOWN") == nullptr);
    const auto& unknown = platform::defaultPlatformProfile();
    assert(unknown.resetQuirk == nullptr);
    assert(!unknown.allowAutomaticInitialization);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cirrus-architecture-check-") as directory:
    tmp = Path(directory)
    source = tmp / "architecture.cpp"
    binary = tmp / "architecture.exe"
    source.write_text(cpp, encoding="utf-8")
    subprocess.run(
        ["g++", "-std=c++17", "-O0", "-I", str(ROOT / "CirrusAudioFixup"), str(source), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("PASS supported silicon registry and declared platform profiles (not discovery behavior)")

# Execute discovery itself: legacy mode permits reads, never invented devices.
start = SOURCE.index("size_t CirrusAudioFixup::detectAmplifiers()")
detect = SOURCE[start:SOURCE.index("\n}", start) + 2]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "Core/AmplifierDescriptors.hpp"
#include "Platform/PlatformProfile.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include "Diagnostics/AttachmentState.hpp"
#include "Diagnostics/TraceTypes.hpp"
using namespace cirrus::diagnostics;
#include "Transport/I2CRegisterBus.hpp"
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
constexpr bool kOSBooleanTrue=true,kOSBooleanFalse=false;
struct Provider {bool getPath(char* out,int*,const void*){strcpy(out,"fixture-controller");return true;}};
const void* gIOServicePlane=nullptr;
struct Controller:cirrus::transport::I2CController {
    unsigned responsive=0,reads=0;
    size_t maxWriteBytes()const override{return 256;}
    size_t maxReadBytes()const override{return 256;}
    IOReturn transfer(const cirrus::transport::I2CEndpoint& e,const uint8_t*,size_t,uint8_t* p,size_t n)override{
        assert(n==4);++reads;if(!(responsive&(1u<<(e.address-0x40))))return kIOReturnNotReady;
        const uint32_t id=0x35a40;for(unsigned i=0;i<4;++i)p[i]=uint8_t(id>>(24-8*i));return 0;
    }
};
class CirrusAudioFixup {
public:
    struct Amp {bool present=false;const char* name=nullptr;uint8_t address=0;uint32_t deviceId=0;
        cirrus::core::AmplifierModel model=cirrus::core::AmplifierModel::CS35L41;
        cirrus::core::AudioChannel channel=cirrus::core::AudioChannel::Left;
        cirrus::core::BusEndpoint endpoint{};uint8_t endpointIndex=0;cirrus::core::RegisterIO* registerIO=nullptr;};
    struct Runtime{bool bound()const{return false;}}mAmplifierManager;
    size_t detectConfiguredI2CAmplifiers(){return 0;}
    size_t expectedAmplifierCount()const{return mPlatformProfile->endpointCount;}
    static constexpr size_t kMaxAmplifiers=2;
    Amp mAmplifiers[2]{};size_t mAmplifierCount=0;
    bool mPlatformAuthorized=false,mProbingAmplifiers=false,legacy=false;
    Controller controller;unsigned& responsive=controller.responsive;unsigned& reads=controller.reads;
    Provider provider;Provider* mProvider=&provider;char mRuntimeControllerPath[256]{};
    cirrus::transport::I2CController* mI2CController=&controller;
    cirrus::transport::I2CRegisterBus* mI2CBuses[2]{};cirrus::core::RegisterMap* mI2CMaps[2]{};
    cirrus::transport::I2CController* createI2CController(){return nullptr;}
    cirrus::platform::AmplifierDriverRegistry mDriverRegistry;
    const cirrus::platform::PlatformProfile* mPlatformProfile=cirrus::platform::findPlatformProfile("CLSA0100");
    CirrusAudioFixup(){
        static const cirrus::platform::AmplifierDriverRegistration d=[](){
            cirrus::core::RegisterMapConfig f;f.readable=[](uint32_t){return true;};f.writable=f.readable;
            cirrus::platform::AmplifierDriverRegistration r{"fixture","CLSA0100",0x35a40,1,f,{},[](const cirrus::platform::AmplifierDriverContext&)->cirrus::core::AmplifierLifecycleHooks*{return nullptr;},[](cirrus::core::AmplifierLifecycleHooks*){}};
            r.model=cirrus::core::AmplifierModel::CS35L41;return r;
        }();assert(mDriverRegistry.add(d));
    }
    ~CirrusAudioFixup(){for(auto* p:mI2CMaps)delete p;for(auto* p:mI2CBuses)delete p;}
    cirrus::diagnostics::AttachmentState mAttachment{};
    void recordTrace(cirrus::diagnostics::TraceSource,uint8_t,bool,bool,uint32_t,uint32_t,IOReturn){}
    void recordAttachmentFailure(const char* s,const char* r,IOReturn e,uint32_t i=0){mAttachment.record(s,r,e,i);}
    void publishAttachmentStatus(){}
    bool bootArgEnabled(const char*){return legacy;}
    template<class... T>void setProperty(T...){}
    size_t detectAmplifiers();
};
''' + detect + r'''
int main() {
    CirrusAudioFixup unknown; unknown.responsive=3;
    assert(unknown.detectAmplifiers()==0 && unknown.reads==0);
    unknown.legacy=true; unknown.responsive=0;
    assert(unknown.detectAmplifiers()==0 && !unknown.mAmplifiers[0].present && !unknown.mAmplifiers[1].present);
    unknown.responsive=2;
    assert(unknown.detectAmplifiers()==1 && unknown.mAmplifiers[0].channel==cirrus::core::AudioChannel::Right);
    CirrusAudioFixup reference; reference.mPlatformAuthorized=true; reference.responsive=3;
    assert(reference.detectAmplifiers()==2);
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-discovery-check-") as directory:
    tmp=Path(directory); source=tmp / "discovery.cpp"; binary=tmp / "discovery.exe"
    source.write_text(fixture)
    from driver_kernel_fixture import install
    install(tmp)
    subprocess.run(["g++","-std=c++17","-I",str(tmp),"-I",str(ROOT / "CirrusAudioFixup"),str(source),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print("PASS production discovery: unknown board, legacy read-only discovery, no phantom endpoints and channel identity")
