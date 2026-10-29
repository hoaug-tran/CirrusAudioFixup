"""Replay GU605MV boot-to-probe timing through the production ACPI/SPI stack.

The OS scheduler, ACPICA entry points and LPSS hardware boundary are fixtures;
the callback, bridge/converter, resource parsers, GPIO CS, native host, framing
and service startup/timer bodies are production code. Reset release and ROM boot
latency are scripted; GPIO reset and DSP lifecycle have separate suites.
This is not a macOS VM or a hardware boot simulation.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tests/Fixtures"))
from acpi_kernel_fixture import install
from host_build import sanitizer_flags
from production_source_fixture import method

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--startup-ref", help="replay historical ACPI/SPI/service files without changing the checkout")
args = parser.parse_args()


def source(name):
    if args.startup_ref:
        return subprocess.check_output(["git", "show", f"{args.startup_ref}:{name}"], cwd=ROOT, text=True)
    return (ROOT / name).read_text()


with tempfile.TemporaryDirectory(prefix="cirrus-boot-replay-") as directory:
    tmp = Path(directory)
    install(tmp)
    fixture = tmp / "Fixture.hpp"
    text = fixture.read_text()
    text = re.sub(r'inline void IOSleep\(unsigned[^)]*\)\{[^}]*\}', 'void IOSleep(unsigned);', text)
    text = text.replace("inline std::function<void(unsigned)> fixtureSleep;\n", "")
    text = text.replace("struct IORegistryEntry:OSObject {std::string path;",
                        "struct IORegistryEntry:OSObject {std::string path;IORegistryEntry* parent=nullptr;")
    text = text.replace("getParentEntry(const void*){return nullptr;}", "getParentEntry(const void*){return parent;}")
    text = text.replace("class IOService:public IORegistryEntry {public:", """
struct OSDictionary;struct OSIterator;
class IOService:public IORegistryEntry {public:
 bool start(IOService*){return true;}void stop(IOService*){}void registerService(){}
 static OSDictionary* serviceMatching(const char*);
 static OSIterator* getMatchingServices(OSDictionary*);
 static IOService* waitForMatchingService(OSDictionary*,uint64_t);
""")
    text = text.replace("class IOPCIDevice:public IOService {};", """
class IOPCIDevice:public IOService {public:
 OSObject* metaCast(const char* n)override{return !strcmp(n,"IOPCIDevice")?this:nullptr;}
 uint16_t configRead16(unsigned o){return o==0?0x8086:0x7e27;}
};
struct OSDictionary:OSObject {
 std::map<std::string,OSObject*> values;
 static OSDictionary* withCapacity(unsigned){auto* p=make<OSDictionary>();p->retain();return p;}
 void setObject(const char* k,OSObject* v){values[k]=v;}
 OSObject* getObject(const char* k){return values[k];}
};
struct OSIterator {std::vector<IOService*> values;size_t index=0;
 OSObject* getNextObject(){return index<values.size()?values[index++]:nullptr;}
 void release(){delete this;}
};
""")
    fixture.write_text(text)
    for header in ("Headers/kern_patcher.hpp", "Support/Logging.hpp", "kern/clock.h", "mach/mach_time.h"):
        path = tmp / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#pragma once\n#include "ReplayOS.hpp"\n')
    # Overlay only the production files involved in the original failure.
    for name in ("Platform/ACPI/ACPIEvaluation.hpp", "Platform/ACPI/ACPIEvaluation.cpp",
                 "Transport/SPI/NativeSPIHost.hpp", "Transport/SPI/Intel/IntelSPIControllerOwner.hpp"):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source("CirrusAudioFixup/" + name))
    (tmp / "ReplayOS.hpp").write_text((ROOT / "Tests/Fixtures/cs35l56_boot_replay_os.hpp").read_text())
    path = tmp / "Transport/SPI/Intel/IntelLPSSSPIController.hpp"
    path.write_text((ROOT / "Tests/Fixtures/cs35l56_boot_replay_lpss.hpp").read_text())
    path = tmp / "Transport/SPI/AMD/AMDSPIControllerOwner.hpp"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('''#pragma once
#include "Transport/SPI/NativeSPIControllerOwner.hpp"
namespace cirrus::transport::amd {
struct AMDSPIController {static bool supportsProvider(IOACPIPlatformDevice*){return false;}};
inline NativeSPIControllerOwner* createNativeSPIController(IOACPIPlatformDevice*,IOPCIDevice*,IOService*,NativeSPIControllerFailure&){assert(false);return nullptr;}
}
''')
    audio = source("CirrusAudioFixup/Platform/HDA/AudioEventSource.cpp")
    begin = audio.index("    void acpiLoaded(")
    callback = audio[begin:audio.index("\n#endif", begin)]
    if "    void acpiPatcherReady(" in audio:
        begin = audio.index("    void acpiPatcherReady(")
        callback = audio[begin:audio.index("\n#endif", begin)]
        callback += "\nvoid runEarlyPatcher(){acpiPatcherReady(nullptr,patcher);}\n"
    else:
        callback += "\nvoid runEarlyPatcher(){}\n"
    (tmp / "Callback.inc").write_text(callback)
    service = source("CirrusAudioFixup/Service/CirrusAudioFixup.cpp")
    runtime = source("CirrusAudioFixup/Service/CirrusAudioFixupRuntime.cpp")
    bodies = "\n".join(method(service, "CirrusAudioFixup", name) for name in
                       ("start", "continueStartup", "finishStartup"))
    bodies += "\n" + method(runtime, "CirrusAudioFixup", "probeTimerFired")
    attachment = source("CirrusAudioFixup/Service/CirrusAudioFixupAttachment.cpp")
    begin = attachment.index("size_t CirrusAudioFixup::detectNativeSPIAmplifiers()")
    bodies += "\n" + attachment[begin:attachment.index("\nvoid CirrusAudioFixup::teardownCommunication()", begin)]
    (tmp / "Service.inc").write_text(bodies)
    (tmp / "Resources.inc").write_text(chr(34) + (ROOT / "Tests/Fixtures/gu605mv_spk1_crs.hex").read_text().strip() + chr(34))
    cpp = tmp / "Replay.cpp"
    cpp.write_text((ROOT / "Tests/Fixtures/cs35l56_boot_replay.cpp").read_text())
    for flags in sanitizer_flags():
        exe = tmp / "replay.exe"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                        *flags, "-DKERNEL", "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(cpp), str(tmp / "Platform/ACPI/ACPIEvaluation.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
