"""Check real C++ mangling for global and namespaced IOKit-style classes."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vtables', ROOT / 'Tools/Build/verify_kext_vtables.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
fixture = '''
class Base { public: virtual void event(); };
class Probe : public Base {
public:
    class MetaClass { public: virtual void event(); };
    static Base* superClass;
    void event() override;
};
Base* Probe::superClass = nullptr;
void Probe::event() {}
void Probe::MetaClass::event() {}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-vtable-policy-') as directory:
    root = Path(directory)
    for nested in (False, True):
        source = root / 'fixture.cpp'
        source.write_text(('namespace audio {\n' + fixture + '\n}') if nested else fixture)
        binary = root / 'fixture.o'
        subprocess.run(['g++', '-c', str(source), '-o', str(binary)], check=True)
        symbols = set(subprocess.check_output(['nm', '-j', str(binary)], text=True).splitlines())
        if sys.platform != 'darwin':
            symbols = {'_' + symbol for symbol in symbols}
        if nested:
            try:
                module.verify_symbols(symbols)
            except ValueError as error:
                assert 'global namespace' in str(error)
            else:
                raise AssertionError('Namespaced metaclass accepted')
        else:
            assert module.verify_symbols(symbols) == 1
            symbols.remove('__ZTVN5Probe9MetaClassE')
            try:
                module.verify_symbols(symbols)
            except ValueError:
                pass
            else:
                raise AssertionError('Missing metaclass vtable accepted')
try:
    module.verify_symbols(set())
except ValueError:
    pass
else:
    raise AssertionError('Stripped/missing symbols accepted')
print('PASS real C++ vtable mangling: global accepted, namespace/missing metaclass/empty rejected')
