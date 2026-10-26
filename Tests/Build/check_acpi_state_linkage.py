"""Catch per-translation-unit ACPI state under the real Darwin kext ABI."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="cirrus-acpi-linkage-") as directory:
    path = Path(directory)
    (path / "IOKit/acpi").mkdir(parents=True)
    (path / "IOKit/acpi/IOACPIPlatformDevice.h").write_text('''
using IOReturn=int;
class OSObject {};
class IOACPIPlatformDevice { public:
void setProperty(const char*,const char*);
void setProperty(const char*,unsigned long long,unsigned);
};
constexpr int kIOReturnBadArgument=-1,kIOReturnNotReady=-2;
''')
    for name in ("setup", "consumer"):
        source = path / f"{name}.cpp"
        source.write_text('''#include "Platform/ACPI/ACPIEvaluation.hpp"
auto* evaluatorAddress(){return &cirrus::transport::nativePropertyEvaluator;}
auto* stateAddress(){return &cirrus::transport::nativePropertySetupState;}
''')
        obj = path / f"{name}.ll"
        subprocess.run(["clang++", "-target", "x86_64-apple-macos10.15", "-std=c++17",
                        "-fapple-kext", "-ffreestanding", "-S", "-emit-llvm", "-I" + directory,
                        "-I" + str(root / "CirrusAudioFixup"), str(source), "-o", str(obj)], check=True)
        symbols = obj.read_text()
        state = [line for line in symbols.splitlines() if "nativeProperty" in line and " = " in line and line.startswith("@")]
        assert len(state) == 2 and all("external global" in line for line in state), symbols
print("PASS Darwin -fapple-kext: setup and consumer reference external ACPI state")
