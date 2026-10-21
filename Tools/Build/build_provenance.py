"""Local build provenance (build host only; standard library)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import datetime


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def snapshot(root):
    # Include tracked deletions and nonignored untracked files, not just HEAD.
    names = set(os.fsdecode(p) for p in git(root, 'ls-files', '-z', '--cached',
                                           '--others', '--exclude-standard').split(b'\0') if p)
    # Dependencies are ignored checkouts but their actual header bytes matter.
    dependencies = {}
    dependency_status = {}
    for name in ('MacKernelSDK', 'Lilu'):
        base = root / name
        if base.exists():
            top = Path(os.fsdecode(git(base, 'rev-parse', '--show-toplevel')).strip()).resolve()
            if top != base.resolve():
                raise ValueError(f'{name} must be an independent dependency checkout for commit provenance')
            dependencies[name] = git(base, 'rev-parse', 'HEAD').decode().strip()
            dependency_status[name] = git(base, 'status', '--porcelain=v1', '--untracked-files=all').decode('utf-8', 'replace')
            for path in base.rglob('*'):
                relative = path.relative_to(base)
                if '.git' not in relative.parts and path.is_file():
                    names.add(path.relative_to(root).as_posix())
    files = []
    for name in sorted(names):
        path = root / name
        if path.is_symlink():
            kind, data = 'symlink', os.fsencode(os.readlink(path))
        elif path.is_file():
            kind, data = 'file', path.read_bytes()
        elif not path.exists():
            kind, data = 'missing', b''
        else:
            continue
        entry = dict(path=name, kind=kind, sha256=hashlib.sha256(data).hexdigest())
        if path.exists():
            entry['executable'] = bool(path.stat().st_mode & 0o111)
        if path.is_symlink() and path.is_file():
            entry['target_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        files.append(entry)
    canonical = json.dumps(files, sort_keys=True, ensure_ascii=True, separators=(',', ':')).encode()
    status = git(root, 'status', '--porcelain=v1', '--untracked-files=all').decode('utf-8', 'replace')
    return dict(schema=1, commit=git(root, 'rev-parse', 'HEAD').decode().strip(),
                dirty=bool(status) or any(dependency_status.values()), status=status, dependencies=dependencies,
                dependency_status=dependency_status,
                source_sha256=hashlib.sha256(canonical).hexdigest(), files=files,
                scope='Tracked and nonignored untracked files, plus actual MacKernelSDK/Lilu files; symlinks hash link text')


def finalize(root, baseline, bundle, configuration, binary_uuid, symbols_uuid):
    current = snapshot(root)
    if current != baseline:
        raise ValueError('Source changed during build; discard this artifact and rebuild')
    if not binary_uuid or binary_uuid != symbols_uuid:
        raise ValueError('Binary/dSYM UUID mismatch')
    binary = bundle / 'Contents/MacOS/CirrusAudioFixup'
    symbols = Path(str(bundle) + '.dSYM') / 'Contents/Resources/DWARF/CirrusAudioFixup'
    manifest = dict(baseline, configuration=configuration, binary_uuid=binary_uuid,
                    dsym_uuid=symbols_uuid, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    dsym_sha256=hashlib.sha256(symbols.read_bytes()).hexdigest(),
                    built_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
    manifest['environment'] = {command: subprocess.check_output(args, text=True).strip() for command, args in (
        ('xcode', ['xcodebuild', '-version']), ('macos', ['sw_vers']), ('kernel', ['uname', '-a']))}
    destination = bundle / 'Contents/Resources/CirrusBuildManifest.plist'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(plistlib.dumps(manifest, sort_keys=True))
    info_path = bundle / 'Contents/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    info.update(CirrusBuildCommit=baseline['commit'], CirrusBuildConfiguration=configuration,
                CirrusBuildDirty=baseline['dirty'], CirrusBuildSourceSHA256=baseline['source_sha256'])
    info_path.write_bytes(plistlib.dumps(info, sort_keys=True))
    print(f"{configuration}: commit={baseline['commit']} dirty={baseline['dirty']} source={baseline['source_sha256']}")
    print(f"binary UUID={binary_uuid} SHA256={manifest['binary_sha256']} manifest={destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('snapshot', 'finalize'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--configuration')
    parser.add_argument('--binary-uuid')
    parser.add_argument('--symbols-uuid')
    args = parser.parse_args()
    if args.action == 'snapshot':
        args.snapshot.write_text(json.dumps(snapshot(args.root), sort_keys=True), encoding='utf-8')
    else:
        finalize(args.root, json.loads(args.snapshot.read_text(encoding='utf-8')), args.bundle,
                 args.configuration, args.binary_uuid, args.symbols_uuid)


if __name__ == '__main__':
    main()
