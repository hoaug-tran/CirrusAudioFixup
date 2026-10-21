"""Compare collector ZIPs offline without extracting or executing their contents.

Usage: python3 Tools/Collectors/compare_macos_evidence.py [--json] boot1.zip boot2.zip ...
Archive order is supplied by the operator, not inferred from filenames.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import zipfile

LIMIT = 64 * 1024 * 1024
FILES = {'services-xml.txt', 'bundle.txt', 'binary-uuid.txt', 'binary-sha256.txt',
         'loaded-kexts.txt', 'loaded-kexts-legacy.txt', 'boot-args.txt',
         'build-manifest.plist', 'collected-at.txt', 'cirrus-fixup-ioreg.txt'}
UUID = r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}'


def normalize(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and (value < 0 or value >= 1 << 63):
        u = value & ((1 << 64) - 1)
        return dict(raw=value, signed64=u - (1 << 64) if u >= 1 << 63 else u,
                    hex64=f'0x{u:016X}', low32_hex=f'0x{u & 0xFFFFFFFF:08X}')
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, dict):
        return {k: normalize(v) for k, v in value.items()}
    if isinstance(value, list):
        return [normalize(v) for v in value]
    return value


def read_zip(path):
    data = {}
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) > 10000:
            raise ValueError('Too many ZIP entries')
        total = 0
        for info in infos:
            name = PurePosixPath(info.filename)
            if name.is_absolute() or '..' in name.parts or '\\' in info.filename:
                raise ValueError('Unsafe ZIP member path')
            if info.is_dir() or name.name not in FILES:
                continue
            if name.name in data:
                raise ValueError('Ambiguous duplicate collector file: ' + name.name)
            if info.flag_bits & 1 or info.file_size > LIMIT:
                raise ValueError('Encrypted or oversized evidence member')
            total += info.file_size
            if total > 2 * LIMIT:
                raise ValueError('Evidence exceeds read budget')
            with archive.open(info) as stream:
                payload = stream.read(LIMIT + 1)
            if len(payload) > LIMIT:
                raise ValueError('Evidence exceeds read budget')
            data[name.name] = payload
    return data


def parse_archive(path):
    data = read_zip(path)
    warnings = []
    def text(name):
        return data.get(name, b'').decode('utf-8', 'replace')
    def plist(name):
        raw = data.get(name, b'')
        if not raw:
            return None
        try:
            start = raw.find(b'<?xml')
            return plistlib.loads(raw[start:] if start >= 0 else raw)
        except Exception as error:
            # Archives are untrusted input; report malformed captures separately.
            warnings.append(f'{name}: invalid plist ({type(error).__name__})')
            return None

    tree = plist('services-xml.txt')
    nodes = [tree] if isinstance(tree, dict) else tree
    live = []
    root_evidence = None
    if isinstance(nodes, list) and nodes and isinstance(nodes[0], dict):
        root_evidence = nodes[0].get('Cirrus_Failed_Service_Evidence')
        def walk(nodes, depth=0):
            if depth > 256:
                raise ValueError('IORegistry tree too deep')
            for node in nodes:
                if not isinstance(node, dict):
                    continue
                if node.get('IOObjectClass') == 'CirrusAudioFixup' or node.get('IOClass') == 'CirrusAudioFixup':
                    live.append(node)
                children = node.get('IORegistryEntryChildren', [])
                if isinstance(children, list):
                    walk(children, depth + 1)
        walk(nodes)
    elif tree is not None:
        warnings.append('services-xml.txt: unexpected root structure')
    if tree is None and re.search(r'<class CirrusAudioFixup[,>]', text('cirrus-fixup-ioreg.txt')):
        # Older slim archives omit XML. Parse only complete scalar property
        # lines; nested objects remain unknown rather than guessed from text.
        scalars = {}
        pattern = r'^\s*"(Cirrus[^"\n]+)"\s*=\s*(?:"([^"\n]*)"|(-?\d+)|(Yes|No))\s*$'
        for match in re.finditer(pattern, text('cirrus-fixup-ioreg.txt'), re.M):
            key, string, integer, boolean = match.groups()
            scalars[key] = string if string is not None else int(integer) if integer is not None else boolean == 'Yes'
        if scalars:
            live.append(scalars)
            warnings.append('XML unavailable: live-service scalar text fallback only; nested/root evidence unknown')
    if root_evidence is not None and not isinstance(root_evidence, dict):
        warnings.append('Root failed evidence is not a dictionary')
        root_evidence = None

    def summarize(properties):
        groups = {name: {} for name in ('first_failure', 'pm', 'reset', 'spi', 'id', 'firmware', 'dsp', 'hda')}
        patterns = dict(first_failure=r'First|Failure|Error|Verdict|Runtime_Stage|Boot_Event|Boot_Checkpoint|Diagnostic_Schema',
                        pm=r'Power|PMCSR|Capability|Wait', reset=r'Reset|GPIO_Group',
                        spi=r'Transaction|Transfer|FirstFrame|_ID_|SPI_MMIO|SPI_Engine|SPI_CS',
                        id=r'DeviceID|Revision|REVID|Speaker|Discovery|Authorized|Detected',
                        firmware=r'Firmware|Tuning|Calibration|WMFW|Coeff', dsp=r'DSP|HALO',
                        hda=r'HDA|Stream|Audio_Event')
        for key, value in properties.items():
            if not key.startswith('Cirrus'):
                continue
            for group, pattern in patterns.items():
                if re.search(pattern, key):
                    groups[group][key] = normalize(value)
        return groups

    manifest = plist('build-manifest.plist')
    if not isinstance(manifest, dict):
        manifest = {}
    # plutil -p capture is not a plist. Only read explicit build fields.
    bundle = text('bundle.txt')
    commit = manifest.get('commit')
    if not commit:
        match = re.search(r'"CirrusBuildCommit"\s*=>\s*"([0-9a-fA-F]{40})"', bundle)
        commit = match.group(1) if match else None
    match = re.search(r'UUID:\s*(' + UUID + ')', text('binary-uuid.txt'))
    supplied_uuid = match.group(1).upper() if match else None
    match = re.search(r'^([0-9a-fA-F]{64})\s', text('binary-sha256.txt'), re.M)
    supplied_sha = match.group(1).lower() if match else None
    loaded = sorted(set(m.group(1).upper() for line in
                        (text('loaded-kexts.txt') + '\n' + text('loaded-kexts-legacy.txt')).splitlines()
                        if re.search(r'\bcom\.hoaugtr\.CirrusAudioFixup\s', line)
                        for m in re.finditer('(' + UUID + ')', line)))
    uuid_state = ('unknown' if not supplied_uuid or not loaded else
                  'match' if loaded == [supplied_uuid] else 'mismatch')
    manifest_uuid = str(manifest.get('binary_uuid', '')).upper() or None
    manifest_sha = manifest.get('binary_sha256')
    manifest_state = ('unknown' if not manifest_uuid or not manifest_sha or not supplied_sha
                      else 'match' if (not supplied_uuid or manifest_uuid == supplied_uuid) and manifest_sha == supplied_sha else 'mismatch')
    loaded_manifest = ('unknown' if manifest_state != 'match' or not loaded else
                       'match' if loaded == [manifest_uuid] else 'mismatch')
    return dict(archive=str(path), boot_args=text('boot-args.txt').split('boot-args\t')[-1].strip() or None,
                collected_at=text('collected-at.txt').strip() or None,
                provenance=dict(bundle_commit=commit, dirty=manifest.get('dirty'),
                                source_sha256=manifest.get('source_sha256'), supplied_uuid=supplied_uuid,
                                supplied_sha256=supplied_sha, loaded_uuids=loaded,
                                loaded_vs_supplied=uuid_state, manifest_vs_supplied=manifest_state,
                                loaded_vs_manifest=loaded_manifest,
                                loaded_source_commit=commit if (uuid_state == 'match' or loaded_manifest == 'match') and manifest_state != 'mismatch' else None),
                services_xml='available' if isinstance(nodes, list) else 'unknown',
                root_failure=summarize(root_evidence) if root_evidence is not None else None,
                live_services=[summarize(node) for node in live], warnings=warnings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('archives', nargs='+', type=Path)
    args = parser.parse_args()
    results = []
    for path in args.archives:
        try:
            results.append(parse_archive(path))
        except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
            results.append(dict(archive=str(path), error=str(error)))
    for previous, current in zip(results, results[1:]):
        if 'error' not in previous and 'error' not in current:
            current['changes_from_previous'] = dict(archive=previous['archive'], sections=[
                key for key in ('boot_args', 'provenance', 'services_xml', 'root_failure', 'live_services')
                if previous[key] != current[key]])
    if args.json:
        print(json.dumps(results, indent=2, sort_keys=True))
    else:
        for result in results:
            print('\n=== ' + result['archive'] + ' ===')
            print(json.dumps({k: v for k, v in result.items() if k != 'archive'}, indent=2, sort_keys=True))
    return int(any('error' in result for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
