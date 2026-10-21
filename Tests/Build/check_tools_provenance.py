"""Execute provenance and offline ZIP comparisons using isolated inputs."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import hashlib
import importlib.util
import json
import os
import plistlib
import shutil
import subprocess
import tempfile
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / ('Tools/Build' if name == 'build_provenance' else 'Tools/Collectors') / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


build = module('build_provenance')
compare = module('compare_macos_evidence')
with tempfile.TemporaryDirectory(prefix='cirrus-provenance-') as directory:
    root = Path(directory)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL)
    git('init', '-q')
    (root / '.gitignore').write_text('build/\nignored\nMacKernelSDK/\nLilu/\n')
    source = root / 'source.cpp'
    source.write_text('original')
    git('add', '.gitignore', 'source.cpp')
    git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
        '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture')
    clean = build.snapshot(root)
    assert not clean['dirty']
    (root / 'ignored').write_text('ignored output')
    assert build.snapshot(root) == clean
    source.write_text('modified')
    dirty = build.snapshot(root)
    assert dirty['commit'] == clean['commit'] and dirty['dirty']
    assert dirty['source_sha256'] != clean['source_sha256']
    git('add', 'source.cpp')
    assert build.snapshot(root)['dirty']
    extra = root / 'new header.hpp'
    extra.write_text('untracked input')
    assert build.snapshot(root)['source_sha256'] != dirty['source_sha256']
    extra.unlink()
    source.unlink()
    assert any(f['kind'] == 'missing' for f in build.snapshot(root)['files'])
    source.write_text('modified')
    dependency = root / 'Lilu'
    dependency.mkdir()
    subprocess.run(['git', 'init', '-q', str(dependency)], check=True)
    (dependency / 'header.h').write_text('dependency')
    subprocess.run(['git', '-C', str(dependency), 'add', 'header.h'], check=True)
    subprocess.run(['git', '-C', str(dependency), '-c', 'user.name=Fixture', '-c',
                    'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false',
                    'commit', '-qm', 'dependency'], check=True, stdout=subprocess.DEVNULL)
    dep_clean = build.snapshot(root)
    (dependency / 'header.h').write_text('dirty dependency')
    dep_dirty = build.snapshot(root)
    assert dep_dirty['dependency_status']['Lilu']
    assert dep_clean['source_sha256'] != dep_dirty['source_sha256']
    bundle = root / 'build/Debug/CirrusAudioFixup.kext'
    binary = bundle / 'Contents/MacOS/CirrusAudioFixup'
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b'binary')
    info = bundle / 'Contents/Info.plist'
    info.write_bytes(plistlib.dumps({'CFBundleVersion': '1.1.0'}))
    symbols = Path(str(bundle) + '.dSYM') / 'Contents/Resources/DWARF/CirrusAudioFixup'
    symbols.parent.mkdir(parents=True)
    symbols.write_bytes(b'symbols')
    snapshot = build.snapshot(root)
    original_check = subprocess.check_output
    def environment(args, **kwargs):
        if args[0] in ('xcodebuild', 'sw_vers', 'uname'):
            return 'fixture environment'
        return original_check(args, **kwargs)
    with patch.object(build.subprocess, 'check_output', side_effect=environment):
        build.finalize(root, snapshot, bundle, 'Debug', 'ABC', 'ABC')
    manifest_path = bundle / 'Contents/Resources/CirrusBuildManifest.plist'
    manifest = plistlib.loads(manifest_path.read_bytes())
    assert manifest['binary_sha256'] == hashlib.sha256(b'binary').hexdigest()
    assert manifest['source_sha256'] == snapshot['source_sha256']
    assert plistlib.loads(info.read_bytes())['CirrusBuildDirty']
    saved = manifest_path.read_bytes()
    for mutate, uuids in ((False, ('ABC', 'DEF')), (True, ('ABC', 'ABC'))):
        if mutate:
            source.write_text('changed while compiling')
        try:
            build.finalize(root, snapshot, bundle, 'Debug', *uuids)
            raise AssertionError('Expected provenance failure')
        except ValueError:
            pass
        assert manifest_path.read_bytes() == saved

    uuid = '417150DE-12B6-3028-9EC2-EEF806972C50'
    stale = '4FD76DC1-E281-3BD1-94E0-F3592730CAFE'
    sha = 'a' * 64
    def archive(files):
        path = root / 'evidence.zip'
        with zipfile.ZipFile(path, 'w') as z:
            for key, value in files.items():
                z.writestr('cirrus-evidence.fixture/' + key, value)
        return path
    result = compare.parse_archive(archive({}))
    assert result['root_failure'] is None and not result['live_services']
    assert result['provenance']['loaded_source_commit'] is None
    assert result['provenance']['dirty'] is None
    slim = compare.parse_archive(archive({'cirrus-fixup-ioreg.txt':
        '+-o Cirrus <class CirrusAudioFixup, id 1>\n  "Cirrus_Platform_Discovery" = "PROPERTIES_INCOMPLETE"\n  "Cirrus_Read_Fail" = 2\n'}))
    assert slim['live_services'][0]['id']['Cirrus_Platform_Discovery'] == 'PROPERTIES_INCOMPLETE'
    assert slim['root_failure'] is None and slim['services_xml'] == 'unknown' and slim['warnings']
    root_node = {'IOObjectClass': 'IORegistryRoot', 'Cirrus_Failed_Service_Evidence': {
        'Cirrus_Attach_First_Reason': 'POWER_TIMEOUT', 'Cirrus_SPI_Power_Return': -536870186,
        'Cirrus_SPI_PMCSR': 18446744073172681430}, 'IORegistryEntryChildren': [
            {'IOObjectClass': 'CirrusAudioFixup', 'Cirrus_Driver_Verdict': 'READ_ONLY'}]}
    files = {'services-xml.txt': b'$ ioreg\n\n' + plistlib.dumps([root_node]),
             'bundle.txt': '"CirrusBuildCommit" => "' + 'b' * 40 + '"',
             'binary-uuid.txt': 'UUID: ' + uuid + ' (x86_64)',
             'binary-sha256.txt': sha + '  supplied/binary',
             'loaded-kexts.txt': 'com.hoaugtr.CirrusAudioFixup (1.1.0) ' + stale}
    result = compare.parse_archive(archive(files))
    assert result['provenance']['loaded_vs_supplied'] == 'mismatch'
    assert result['provenance']['loaded_source_commit'] is None
    assert len(result['live_services']) == 1
    files['services-xml.txt'] = plistlib.dumps(root_node)
    assert compare.parse_archive(archive(files))['root_failure'] == result['root_failure']
    failure = result['root_failure']
    assert failure['first_failure']['Cirrus_Attach_First_Reason'] == 'POWER_TIMEOUT'
    assert failure['pm']['Cirrus_SPI_Power_Return']['signed64'] == -536870186
    assert failure['pm']['Cirrus_SPI_PMCSR']['low32_hex'] == f'0x{18446744073172681430 & 0xFFFFFFFF:08X}'
    files['loaded-kexts.txt'] = 'com.hoaugtr.CirrusAudioFixup (1.1.0) ' + uuid
    files['build-manifest.plist'] = plistlib.dumps(dict(commit='b' * 40, dirty=True,
        source_sha256='c' * 64, binary_uuid=uuid, binary_sha256=sha))
    result = compare.parse_archive(archive(files))
    assert result['provenance']['manifest_vs_supplied'] == 'match'
    assert result['provenance']['loaded_source_commit'] == 'b' * 40
    saved_uuid = files.pop('binary-uuid.txt')
    assert compare.parse_archive(archive(files))['provenance']['loaded_vs_manifest'] == 'match'
    files['binary-uuid.txt'] = saved_uuid
    files['binary-sha256.txt'] = 'd' * 64 + '  replaced/binary'
    result = compare.parse_archive(archive(files))
    assert result['provenance']['manifest_vs_supplied'] == 'mismatch'
    assert result['provenance']['loaded_source_commit'] is None
    files['services-xml.txt'] = b'truncated <?xml'
    assert compare.parse_archive(archive(files))['warnings']
    files['services-xml.txt'] = plistlib.dumps([{'IOObjectClass': 'IORegistryRoot'}])
    assert compare.parse_archive(archive(files))['root_failure'] is None
    unsafe = root / 'unsafe.zip'
    with zipfile.ZipFile(unsafe, 'w') as z:
        z.writestr('../services-xml.txt', b'ignored')
    try:
        compare.parse_archive(unsafe)
        raise AssertionError('Unsafe path accepted')
    except ValueError:
        pass
    with zipfile.ZipFile(unsafe, 'w') as z:
        z.writestr('one/bundle.txt', 'first')
        z.writestr('two/bundle.txt', 'ambiguous')
    try:
        compare.parse_archive(unsafe)
        raise AssertionError('Duplicate collector file accepted')
    except ValueError:
        pass
    with patch.object(compare, 'LIMIT', 2):
        try:
            compare.parse_archive(archive({'bundle.txt': 'oversized'}))
            raise AssertionError('Oversized evidence accepted')
        except ValueError:
            pass
    path = archive(files)
    cli = subprocess.run([sys.executable, str(ROOT / 'Tools/Collectors/compare_macos_evidence.py'),
                          '--json', str(path), str(path)], capture_output=True, text=True, check=True)
    assert len(json.loads(cli.stdout)) == 2

    # Execute collector helpers, using real plutil on macOS. Elsewhere the
    # fixture implements only its plist CLI contract; no macOS runtime claim.
    bash = shutil.which('bash')
    if sys.platform == 'win32':
        bash = 'C:/Program Files/Git/bin/bash.exe'
    collector = (ROOT / 'Tools/Collectors/collect_macos.sh').read_text(encoding='utf-8')
    helpers = collector[collector.index('extract_failed_evidence() {'):collector.index('capture system ')]
    stub = root / 'plutil_fixture.py'
    stub.write_text('''import sys,plistlib,json
try:
    args=sys.argv[1:]; obj=plistlib.loads(open(args[-1],'rb').read())
    if args[0]=='-lint': pass
    elif args[0]=='-extract':
        for key in args[1].split('.'):
            obj=obj[int(key)] if isinstance(obj,list) else obj[key]
        if args[2]=='xml1': open(args[4],'wb').write(plistlib.dumps(obj))
        else: print(obj)
    elif args[0]=='-p':
        print('{')
        for k,v in obj.items():
            print('  '+json.dumps(k)+' => '+('{' if isinstance(v,dict) else '[' if isinstance(v,list) else json.dumps(v)))
        print('}')
except Exception: sys.exit(1)
''', encoding='utf-8')
    preamble = ''
    if sys.platform != 'darwin':
        helpers = helpers.replace('/usr/bin/plutil', 'fixture_plutil')
        helpers = helpers.replace('/usr/bin/shasum', 'fixture_shasum')
        preamble = '''fixture_plutil(){ "$TEST_PYTHON" "$TEST_PLUTIL" "$@"; }
fixture_shasum(){ "$TEST_PYTHON" -c 'import sys,hashlib; print(hashlib.sha256(open(sys.argv[-1],"rb").read()).hexdigest()+"  binary")' "$@"; }
'''
    env = dict(os.environ, TEST_PYTHON=Path(sys.executable).as_posix(), TEST_PLUTIL=stub.as_posix())
    script = preamble + 'set -o pipefail\noutput=.\n' + helpers
    for value, expected, code in ((root_node, 'POWER_TIMEOUT', 0),
                                  ([root_node], 'POWER_TIMEOUT', 0),
                                  ({'Cirrus_Failed_Service_Evidence':{}}, 'No top-level scalar', 0),
                                  ({'IOObjectClass':'IORegistryRoot'}, 'NOT_PRESENT', 0),
                                  ({'Cirrus_Failed_Service_Evidence':['wrong type']}, 'INVALID', 1),
                                  (None, 'UNAVAILABLE', 1)):
        (root / 'services-xml.txt').write_bytes(b'$ ioreg\n\n' + (plistlib.dumps(value) if value is not None else b'broken XML'))
        result = subprocess.run([bash, '-c', script + '\nextract_failed_evidence\n'], cwd=root, env=env,
                                capture_output=True, text=True)
        assert result.returncode == code, result.stderr
        assert expected in (root / 'failed-service-summary.txt').read_text()
        assert (root / 'services-xml.txt').read_bytes().startswith(b'$ ioreg')
    binary.write_bytes(b'binary')
    manifest_path.write_bytes(plistlib.dumps(dict(binary_uuid=uuid,
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())))
    # Relative arguments keep Git Bash and Windows Python paths consistent.
    binary_arg = binary.relative_to(root).as_posix()
    manifest_arg = manifest_path.relative_to(root).as_posix()
    command = script + f'\nverify_build_provenance "{manifest_arg}" "{binary_arg}"\n'
    for loaded_uuid, expected in ((uuid, 'LOADED_UUID_MATCH'), (stale, 'LOADED_UUID_UNCONFIRMED')):
        (root / 'loaded-kexts.txt').write_text('com.hoaugtr.CirrusAudioFixup (1.1.0) ' + loaded_uuid)
        (root / 'loaded-kexts-legacy.txt').write_text('')
        result = subprocess.run([bash, '-c', command], cwd=root, env=env, capture_output=True, text=True)
        assert result.returncode == 0 and expected in result.stdout, result.stderr + result.stdout
    binary.write_bytes(b'replaced')
    result = subprocess.run([bash, '-c', command], cwd=root, env=env, capture_output=True, text=True)
    assert result.returncode == 1 and 'MISMATCH_OR_INVALID' in result.stdout

print('PASS build source/dirty/dependency snapshots, mutation/UUID rejection, offline ZIP provenance and failure separation')
print('PASS collector extraction/provenance helpers (native plutil on macOS; contract fixture elsewhere)')
