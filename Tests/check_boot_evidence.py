"""Exercise the production retained boot journal, including first-error limits."""
from pathlib import Path
import subprocess
import tempfile
from acpi_kernel_fixture import install

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "CirrusAudioFixup/CirrusAudioFixup.cpp").read_text()
start = source.index("    void publishBootCheckpoint(")
checkpoint = source[start:source.index("    void retainFailedServiceEvidence(", start)]
with tempfile.TemporaryDirectory(prefix="cirrus-boot-evidence-") as directory:
    tmp = Path(directory)
    install(tmp)
    fixture = tmp / "Fixture.hpp"
    text = fixture.read_text()
    text = text.replace('strings[k]=v;return true;', 'strings[k]=v;objects[k]=make<OSString>(v);return true;')
    text = text.replace('numbers[k]=v;return true;', 'numbers[k]=v;objects[k]=make<OSNumber>(v);return true;')
    fixture.write_text(text)
    cpp = tmp / "test.cpp"
    cpp.write_text(r'''
#include "Fixture.hpp"
bool gCirrusDebug=true;
uint64_t clockNs=1000;
uint64_t mach_absolute_time(){return clockNs+=1000;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t* value){*value=t;}
''' + checkpoint + r'''
int main(){
 IOService owner;
 publishBootCheckpoint(nullptr,"NULL",kIOReturnError);
 gCirrusDebug=false;publishBootCheckpoint(&owner,"DISABLED",kIOReturnError);
 assert(owner.objects.empty());gCirrusDebug=true;
 publishBootCheckpoint(&owner,"COMMUNICATION_BEGIN");
 publishBootCheckpoint(&owner,"DOMAIN_TIMEOUT",kIOReturnTimeout);
 publishBootCheckpoint(&owner,"SHUTDOWN");
 publishBootCheckpoint(&owner,"CLEANUP_FAILURE",kIOReturnIOError);
 assert(owner.strings["Cirrus_Boot_First_Error"]=="DOMAIN_TIMEOUT");
 assert(owner.numbers["Cirrus_Boot_First_Return"]==uint32_t(kIOReturnTimeout));
 assert(owner.strings["Cirrus_Boot_Checkpoint_001"].find("DOMAIN_TIMEOUT")!=std::string::npos);
 for(unsigned i=0;i<150;++i)publishBootCheckpoint(&owner,"POLL");
 assert(owner.numbers["Cirrus_Boot_Checkpoint_Count"]==96);
 assert(!owner.strings.count("Cirrus_Boot_Checkpoint_096"));
 assert(owner.strings["Cirrus_Boot_Checkpoint_Status"]=="PARTIAL_LIMIT");
 assert(owner.strings["Cirrus_Boot_First_Error"]=="DOMAIN_TIMEOUT");
 assert(owner.strings["Cirrus_Boot_Checkpoint_Last"]=="POLL");
}
''')
    exe = tmp / "test.exe"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS retained boot evidence: first failure survives cleanup, bounded journal and disabled logging")
