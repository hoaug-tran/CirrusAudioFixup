"""Check embedded tuning parsing against explicit layout and malformed cases."""
from pathlib import Path
import importlib.util
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
driver = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
assert 'tuning::parse(tuningData, tuningSize, tuning)' in driver
assert 'tuning::encodeGain(amp.tuningPcmGain)' in driver
for forbidden in ('OSKextRequestResource(', 'vnode_open(', 'vn_rdwr('):
    assert forbidden not in driver, 'Runtime firmware must remain embedded'
spec = importlib.util.spec_from_file_location('firmware_importer', ROOT / 'Tools/import_firmware.py')
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)

def record(kind, payload=b'', index=0):
    return struct.pack('<3I', index, kind, 12 + len(payload)) + payload

def container(*records):
    body = b''.join(records)
    return struct.pack('<4I', 0x109A4A35, 1, 16 + len(body), len(records)) + body

cases = [(container(), True, 17)]
for gain in range(21):
    cases.append((container(record(0, struct.pack('<I', gain))), True, gain))
cases += [
    (container(record(8), record(0, struct.pack('<I', 14), 9)), True, 14),
    (container(record(0, struct.pack('<I', 21))), False, 17),
    (container(record(0)), False, 17),
    (container(record(0, bytes(8))), False, 17),
    (container(record(0, bytes(4)), record(0, bytes(4))), False, 17),
    (container(record(7)), True, 17),
]
valid = container(record(0, struct.pack('<I', 14)))
for length in range(len(valid)):
    cases.append((valid[:length], False, 17))
for offset, word in [(0, 0), (4, 2), (8, 0), (12, 2), (24, 0), (24, 0xFFFFFFFF), (24, 15)]:
    data = bytearray(valid)
    struct.pack_into('<I', data, offset, word)
    cases.append((bytes(data), False, 17))
cases.append((valid + bytes(4), False, 17))

checks = []
for index, (data, valid_expected, gain) in enumerate(cases):
    ok, message = importer.validate_bincfg(data)
    assert ok == valid_expected, (index, message)
    initializer = ','.join(str(byte) for byte in data) or '0'
    checks.append(f'''{{
        uint8_t data[] = {{{initializer}}};
        Parameters result; result.pcmGain=20; result.overridden=true;
        assert(parse(data,{len(data)},result)=={str(valid_expected).lower()});
        assert(result.pcmGain=={gain});
        if (!{str(valid_expected).lower()}) assert(!result.overridden);
    }}''')
source = '''
#include <cassert>
#include "Devices/CS35L41/Resources/Tuning.hpp"
using namespace cirrus::devices::cs35l41::tuning;
int main() {
    Parameters result;
    assert(parse(nullptr,0,result) && result.pcmGain==17 && !result.overridden);
    assert(!parse(nullptr,32,result));
    uint8_t byte=0;
    assert(!parse(&byte,0,result));
    assert(!parse(&byte,65540,result));
    assert(encodeGain(17)==0x233);
''' + '\n'.join(checks) + '\n}\n'
with tempfile.TemporaryDirectory(prefix='cirrus-tuning-') as directory:
    tmp = Path(directory)
    cpp = tmp / 'tuning.cpp'
    cpp.write_text(source, encoding='utf-8')
    exe = tmp / 'tuning.exe'
    subprocess.run(['g++', '-std=c++17', '-O0', '-I', str(ROOT / 'CirrusAudioFixup'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print(f'PASS embedded tuning: {len(cases)} layout/range/truncation cases, transactional parsing and importer agreement')
