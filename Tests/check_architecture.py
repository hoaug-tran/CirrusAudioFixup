from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text(encoding="utf-8")

cpp = r'''
#include <cassert>
#include <cstring>
#include "Core/DeviceRegistry.hpp"
#include "Platform/PlatformProfile.hpp"

int main() {
    using namespace cirrus;

    const auto* cs35l41 = core::findDeviceById(0x00035A40);
    assert(cs35l41);
    assert(cs35l41->model == core::CodecModel::CS35L41);
    assert(cs35l41->hasHaloDsp);
    assert(core::findDeviceById(0x00000000) == nullptr);
    assert(core::supportsDevice(core::CodecModel::CS35L41));
    assert(!core::supportsDevice(core::CodecModel::CS35L56));

    const auto* lenovo = platform::findPlatformProfile("CLSA0100");
    assert(lenovo);
    assert(lenovo->amplifierModel == core::CodecModel::CS35L41);
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
fixture = r'''#include <cassert>
#include <cstdint>
#include <cstddef>
#include "Core/DeviceRegistry.hpp"
#include "Platform/PlatformProfile.hpp"
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
constexpr bool kOSBooleanTrue=true, kOSBooleanFalse=false;
class CirrusAudioFixup {
public:
    struct Amp { bool present=false; const char* name=nullptr; uint8_t address=0; uint32_t deviceId=0;
        cirrus::core::CodecModel model=cirrus::core::CodecModel::CS35L41;
        cirrus::core::AudioChannel channel=cirrus::core::AudioChannel::Left; };
    size_t detectConfiguredI2CAmplifiers(){return 0;}
    static constexpr size_t kMaxAmps=2;
    Amp mAmps[2]; size_t mAmpCount=0;
    bool mPlatformAuthorized=false,mProbingAmplifiers=false,legacy=false;
    unsigned responsive=0,reads=0;
    const cirrus::platform::PlatformProfile* mPlatformProfile=cirrus::platform::findPlatformProfile("CLSA0100");
    bool bootArgEnabled(const char*) { return legacy; }
    template<class... T> void setProperty(T...) {}
    bool transferToAddress(uint8_t address,uint8_t*,size_t,uint8_t* data,size_t) {
        ++reads; if(!(responsive & (1u << (address-0x40)))) return false;
        data[0]=0; data[1]=3; data[2]=0x5A; data[3]=0x40; return true;
    }
    size_t detectAmplifiers();
};
''' + detect + r'''
int main() {
    CirrusAudioFixup unknown; unknown.responsive=3;
    assert(unknown.detectAmplifiers()==0 && unknown.reads==0);
    unknown.legacy=true; unknown.responsive=0;
    assert(unknown.detectAmplifiers()==0 && !unknown.mAmps[0].present && !unknown.mAmps[1].present);
    unknown.responsive=2;
    assert(unknown.detectAmplifiers()==1 && unknown.mAmps[0].channel==cirrus::core::AudioChannel::Right);
    CirrusAudioFixup reference; reference.mPlatformAuthorized=true; reference.responsive=3;
    assert(reference.detectAmplifiers()==2);
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-discovery-check-") as directory:
    tmp=Path(directory); source=tmp / "discovery.cpp"; binary=tmp / "discovery.exe"
    source.write_text(fixture)
    subprocess.run(["g++","-std=c++17","-I",str(ROOT / "CirrusAudioFixup"),str(source),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print("PASS production discovery: unknown board, legacy read-only discovery, no phantom endpoints and channel identity")
