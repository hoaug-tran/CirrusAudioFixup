"""Validate Xcode navigation paths, source membership, and the build gate."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]
from xcode_project import parse_project
project_path = ROOT / 'CirrusAudioFixup.xcodeproj/project.pbxproj'
project = parse_project(project_path.read_text())
if sys.platform == 'darwin':
    native = json.loads(subprocess.check_output([
        'plutil', '-convert', 'json', '-o', '-', str(project_path)], text=True))
    def canonical(value):
        if isinstance(value, dict):
            return {key: canonical(item) for key, item in value.items()}
        if isinstance(value, list):
            return [canonical(item) for item in value]
        return str(value)
    assert canonical(project) == canonical(native), "Portable project reader disagrees with plutil"
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
expected = {path for path in (ROOT / 'CirrusAudioFixup').rglob('*') if path.is_file() and path.name != '.DS_Store'}
assert expected <= set(references.values()), expected - set(references.values())
target = objects['CAFE00000040']
compiled = set()
for identifier in target['buildPhases']:
    phase = objects[identifier]
    if phase['isa'] == 'PBXSourcesBuildPhase':
        compiled.update(references[objects[item]['fileRef']] for item in phase['files'])
assert compiled == set((ROOT / 'CirrusAudioFixup').rglob('*.cpp'))
assert len(compiled) == 16

gates = [objects[i] for i in target['buildPhases']
         if objects[i]['isa'] == 'PBXShellScriptBuildPhase']
assert len(gates) == 1 and gates[0]['alwaysOutOfDate'] == '1'
assert 'verify_kext_vtables.py' in gates[0]['shellScript']
assert '${TARGET_BUILD_DIR}/${EXECUTABLE_PATH}' in gates[0]['shellScript']
print('PASS Xcode: every source/header visible, paths resolve, source membership preserved, vtable gate installed')
