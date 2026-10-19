"""Compare register/OTP/errata data with an explicitly supplied Linux source.

Usage: python Tools/audit_linux_reference.py --reference-dir DIR --output FILE
No networking, source modification or hardware accesses are performed.
Unresolved symbols are reported as uncovered, never counted as matching.
"""
from pathlib import Path
import argparse
import ast
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-dir', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
reference = args.reference_dir
header = (reference/'cs35l41.h').read_text()
library = (reference/'cs35l41-lib.c').read_text()
macros = dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)', header, re.M))
if (reference/'cs_dsp.c').exists():
    macros.update(re.findall(r'^#define\s+(\w+)\s+([^\n]+)', (reference/'cs_dsp.c').read_text(), re.M))
macros.update(re.findall(r'^\s*(CSPL_\w+)\s*=\s*([^,\n]+),', header, re.M))


def number(expression, values):
    expression = re.sub(r'/\*.*?\*/', '', expression).strip()
    expression = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', expression)
    node = ast.parse(expression, mode='eval').body

    def resolve(n):
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return n.value
        if isinstance(n, ast.Name):
            return number(values[n.id], values)
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name):
            a = [resolve(v) for v in n.args]
            if n.func.id == 'BIT' and len(a) == 1:
                return 1 << a[0]
            if n.func.id == 'GENMASK' and len(a) == 2:
                return ((1 << (a[0]+1))-1) ^ ((1 << a[1])-1)
        if isinstance(n, ast.BinOp):
            a, b = resolve(n.left), resolve(n.right)
            for kind, operation in ((ast.LShift, lambda: a << b), (ast.RShift, lambda: a >> b),
                                    (ast.BitOr, lambda: a | b), (ast.BitAnd, lambda: a & b),
                                    (ast.Add, lambda: a+b), (ast.Sub, lambda: a-b)):
                if isinstance(n.op, kind):
                    return operation()
        raise ValueError(expression)
    return resolve(node)


def array(source, name):
    match = re.search(r'\b'+re.escape(name)+r'(?:\[[^\]]*\])+\s*=\s*\{', source)
    if not match:
        raise ValueError('Missing array '+name)
    depth = 1
    end = match.end()
    while end < len(source) and depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if depth:
        raise ValueError('Unterminated array '+name)
    text = re.sub(r'/\*.*?\*/', '', source[match.end():end-1], flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    rows = re.findall(r'\{([^{}]+)\}', text)
    return [tuple(number(v.strip(), macros) for v in row.split(',') if v.strip())
            for row in (rows or [text])]


registers = (ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/Hardware/Registers.hpp').read_text()
constants = dict(re.findall(r'constexpr\s+(?:uint8_t|uint16_t|uint32_t)\s+(\w+)\s*=\s*([^;]+);', registers))
registry = ast.parse((ROOT/'Tests/check_registers.py').read_text())
assignments = {n.targets[0].id: ast.literal_eval(n.value) for n in registry.body
               if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name)
               and n.targets[0].id in ('LEGACY_MACROS', 'SPECIAL_NAMES')}
comparisons, uncovered, failures = [], [], []
aliases = {'CS35L41_SW_RESET': 'CS35L41_SFT_RESET', 'HALO_CORE_RESET': 'CS35L41_HALO_CORE_RESET',
           'HALO_CORE_EN': 'HALO_CORE_EN', 'CS35L41_SW_RESET_VAL': 'CS35L41_SOFTWARE_RESET'}
macros['CS35L41_PROTECTION_MASK'] = 'CS35L41_AMP_SHORT_ERR | CS35L41_BST_SHORT_ERR | CS35L41_TEMP_WARN | CS35L41_TEMP_ERR | CS35L41_BST_OVP_ERR | CS35L41_BST_DCM_UVP_ERR'
for linux_name, _ in assignments['LEGACY_MACROS']:
    upstream = aliases.get(linux_name, linux_name)
    local = assignments['SPECIAL_NAMES'].get(linux_name)
    if local is None:
        name = re.sub(r'^(CS35L41_|CSPL_|HALO_)', '', linux_name)
        local = 'k'+''.join(p.capitalize() if not p.isdigit() else p for p in name.split('_'))
    if upstream not in macros or local not in constants:
        uncovered.append(dict(upstream=upstream, local=local, reason='Not a directly mapped header macro'))
        continue
    actual, expected = number(constants[local], constants), number(macros[upstream], macros)
    comparisons.append(dict(local=local, upstream=upstream, actual=hex(actual), expected=hex(expected)))
    if actual != expected:
        failures.append(local)
actual = number(constants['kLastRegister'], constants)
expected = number(macros['CS35L41_LASTREG'], macros)
comparisons.append(dict(local='kLastRegister', upstream='CS35L41_LASTREG', actual=hex(actual), expected=hex(expected)))
if actual != expected:
    failures.append('kLastRegister')
descriptor_comparisons = []
for address, name in re.findall(r'\{\s*(0x[\da-fA-F]+),\s*"(CS35L41_\w+)"', registers):
    if name not in macros:
        uncovered.append(dict(upstream=name, local=address, reason='Descriptor has no header macro'))
        continue
    actual, expected = int(address, 0), number(macros[name], macros)
    descriptor_comparisons.append(dict(name=name, actual=hex(actual), expected=hex(expected)))
    if actual != expected:
        failures.append(name)

checks = []
otp = (ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/Hardware/OTPMap.hpp').read_text()
for index in (1, 2):
    a, b = array(otp, 'kOtpMap'+str(index)), array(library, 'otp_map_'+str(index))
    checks.append(dict(name='OTP map '+str(index), entries=len(a), upstreamEntries=len(b), match=a == b))
errata = (ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/Hardware/Errata.hpp').read_text()
a, b = array(errata, 'kCs35l41RevB2ErrataPatch'), array(library, 'cs35l41_revb2_errata_patch')
checks.append(dict(name='B2 errata', entries=len(a), upstreamEntries=len(b), match=a == b))
power = (ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/PowerConfiguration.hpp').read_text()
for local, upstream in (('k1', 'cs35l41_bst_k1_table'), ('k2', 'cs35l41_bst_k2_table')):
    a, b = array(power, local), array(library, upstream)
    checks.append(dict(name='Boost '+local, entries=sum(map(len, a)), match=a == b))
a, b = array(power, 'slope'), array(library, 'cs35l41_bst_slope_table')
checks.append(dict(name='Boost slope', entries=sum(map(len, a)), match=a == b))
failures += [c['name'] for c in checks if not c['match']]

# Every numeric literal is inventoried, but occurrence is not proof of correct
# semantics. Firmware bytes are verified separately by payload hashes/parsers.
inventory = []
for path in sorted((ROOT/'CirrusAudioFixup').rglob('*')):
    if path.suffix not in ('.hpp', '.cpp') or path.name == 'Firmware.hpp':
        continue
    for line, text in enumerate(path.read_text().splitlines(), 1):
        for literal in re.findall(r'\b(?:0[xX][0-9a-fA-F]+|\d+)(?:[uUlL]+)?\b', text.split('//')[0]):
            inventory.append(dict(file=path.relative_to(ROOT).as_posix(), line=line, literal=literal))
result = dict(referenceFiles=[dict(name=p.name, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                              for p in sorted(reference.glob('*')) if p.suffix in ('.c', '.h')],
              registerComparisons=comparisons, descriptorComparisons=descriptor_comparisons,
              uncoveredMacros=uncovered, tables=checks,
              literalOccurrences=inventory, failures=failures,
              limitations='Literal inventory is not semantic or hardware certification; unmapped symbols are uncovered.')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2)+'\n')
print(f'{len(comparisons)} register + {len(descriptor_comparisons)} descriptor comparisons; tables={checks}; uncovered={len(uncovered)}; '
      f'literal occurrences={len(inventory)}; failures={failures}')
raise SystemExit(bool(failures))
