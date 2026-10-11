"""Validate Xcode navigation paths, source membership, and the build gate."""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if sys.platform != 'darwin':
    print('SKIP Xcode project parser: macOS plutil required')
    raise SystemExit(0)
project = json.loads(subprocess.check_output([
    'plutil', '-convert', 'json', '-o', '-',
    str(ROOT / 'CirrusAudioFixup.xcodeproj/project.pbxproj')], text=True))
objects = project['objects']
references = {}

def walk(identifier, parent):
    entry = objects[identifier]
    if entry.get('sourceTree') == 'BUILT_PRODUCTS_DIR':
        return
    path = parent / entry.get('path', '')
    if entry['isa'] == 'PBXGroup':
        assert path.is_dir(), path
        for child in entry['children']:
            walk(child, path)
    elif entry['isa'] == 'PBXFileReference':
        assert path.is_file(), path
        references[identifier] = path

walk(objects[project['rootObject']]['mainGroup'], ROOT)
expected = {path for path in (ROOT / 'CirrusAudioFixup').rglob('*') if path.is_file()}
assert expected <= set(references.values()), expected - set(references.values())
target = objects['CAFE00000040']
compiled = set()
for identifier in target['buildPhases']:
    phase = objects[identifier]
    if phase['isa'] == 'PBXSourcesBuildPhase':
        compiled.update(references[objects[item]['fileRef']] for item in phase['files'])
assert compiled == {ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp',
                    ROOT / 'CirrusAudioFixup/Platform/HDA/AudioEvents.cpp',
                    ROOT / 'CirrusAudioFixup/Transport/Intel/IntelLPSSSPIController.cpp'}
gates = [objects[i] for i in target['buildPhases']
         if objects[i]['isa'] == 'PBXShellScriptBuildPhase']
assert len(gates) == 1 and gates[0]['alwaysOutOfDate'] == '1'
assert 'verify_kext_vtables.py' in gates[0]['shellScript']
assert '${TARGET_BUILD_DIR}/${EXECUTABLE_PATH}' in gates[0]['shellScript']
print('PASS Xcode: every source/header visible, paths resolve, source membership preserved, vtable gate installed')
