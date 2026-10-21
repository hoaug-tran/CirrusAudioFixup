"""Exercise the production checked name cast with unavailable imported identity."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = r'''
#include <cassert>
#include <cstring>
struct Base { virtual ~Base()=default; virtual Base* metaCast(const char*) { return nullptr; } };
class IOACPIPlatformDevice : public Base {
public:
 Base* metaCast(const char* name) override { return strcmp(name,"IOACPIPlatformDevice") == 0 ? this : nullptr; }
};
class IOPCIDevice : public Base { public: Base* metaCast(const char* name) override { return strcmp(name,"IOPCIDevice") == 0 ? this : nullptr; } };
struct ACPIChild : IOACPIPlatformDevice {};
struct NonACPI : Base {};
#define KERNEL 1
#include "Support/KernelDeviceCast.hpp"
int main() {
 IOACPIPlatformDevice acpi; ACPIChild child; NonACPI nub;
 Base* provider=&acpi;
 assert(cirrus::support::acpiDeviceCast(provider)==&acpi);
 provider=&child; assert(cirrus::support::acpiDeviceCast(provider)==&child);
 provider=&nub; assert(cirrus::support::acpiDeviceCast(provider)==nullptr);
 provider=nullptr; assert(cirrus::support::acpiDeviceCast(provider)==nullptr);
 IOPCIDevice pci; provider=&pci; assert(cirrus::support::pciDeviceCast(provider)==&pci);
 provider=&acpi; assert(cirrus::support::pciDeviceCast(provider)==nullptr);
 provider=nullptr; assert(cirrus::support::pciDeviceCast(provider)==nullptr);
}
'''
with tempfile.TemporaryDirectory() as folder:
    cpp=Path(folder)/'cast.cpp'; binary=Path(folder)/'cast'; cpp.write_text(source)
    (Path(folder)/'IOKit/acpi').mkdir(parents=True)
    (Path(folder)/'IOKit/pci').mkdir(parents=True)
    (Path(folder)/'IOKit/pci/IOPCIDevice.h').write_text('')
    (Path(folder)/'IOKit/IORegistryEntry.h').write_text('')
    (Path(folder)/'IOKit/acpi/IOACPIPlatformDevice.h').write_text('')
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I',folder,'-I',str(root/'CirrusAudioFixup'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS registered ACPI hierarchy cast: base, subclass, unrelated provider and null; no imported metaclass pointer')
