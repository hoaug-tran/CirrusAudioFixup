"""Check firmware CLI failures and collector structure without claiming macOS runtime coverage."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
tool = ROOT / "Tools/import_firmware.py"
with tempfile.TemporaryDirectory(prefix="cirrus-tools-") as directory:
    tmp = Path(directory)
    firmware = tmp / "cs35l41.wmfw"
    left = tmp / "left.bin"
    right = tmp / "right.bin"
    firmware.write_bytes(b"WMFW" + struct.pack("<IHBB", 40, 0, 4, 2) + bytes(28))
    left.write_bytes(b"WMDR" + bytes(12))
    right.write_bytes(left.read_bytes())
    command = [sys.executable, str(tool), "--codec", "cs35l41", "--ssid", "0x17aa3847",
               "--wmfw", str(firmware), "--bin-l", str(left), "--bin-r", str(left), "--spkid", "1"]
    def run(arguments, expected):
        result = subprocess.run(command + arguments, capture_output=True, text=True)
        assert (result.returncode == 0) == expected, result.stdout + result.stderr
    run(["--validate-only"], True)
    run(["--bin-r", str(right), "--validate-only"], True)
    right.write_bytes(b"invalid")
    run(["--bin-r", str(right), "--validate-only"], False)
    missing = tmp / "missing.bin"
    run(["--bin-r", str(missing), "--validate-only"], False)
    # Write mode must reject the missing channel before it can touch the header.
    header = tmp / "Firmware.hpp"
    header.write_text("unchanged", encoding="utf-8")
    run(["--bin-r", str(missing), "--firmware", str(header)], False)
    assert header.read_text(encoding="utf-8") == "unchanged"
    gain_left = tmp / "left.bincfg"
    gain_right = tmp / "right.bincfg"
    def gain_data(code):
        return struct.pack('<8I', 0x109A4A35, 1, 32, 1, 0, 0, 16, code)
    gain_left.write_bytes(gain_data(14))
    gain_right.write_bytes(gain_data(16))
    run(["--bincfg-l", str(gain_left), "--bincfg-r", str(gain_right), "--validate-only"], True)
    run(["--bincfg-r", str(missing), "--validate-only"], False)
    gain_right.write_bytes(gain_data(21))
    run(["--bincfg-r", str(gain_right), "--validate-only"], False)
    gain_right.write_bytes(gain_data(16))
    header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--bincfg-l", str(gain_left), "--bincfg-r", str(gain_right), "--firmware", str(header)], True)
    imported = header.read_text(encoding='utf-8')
    assert 'SHA256:' in imported and 'l0_bincfg' in imported and 'r0_bincfg' in imported
    def compile_profile(expected_right):
        declarations = (ROOT / 'CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/ResourceSchema.hpp').read_text(encoding='utf-8')
        resource = declarations[declarations.index('struct FirmwareResource {'):len(declarations)]
        cpp = tmp / 'profile.cpp'
        cpp.write_text('#include <cassert>\n#include <cstdint>\n#include <cstddef>\n'
                       '#include "Devices/Amplifiers/CS35L41/Resources/Tuning.hpp"\n' + resource + imported + f'''
int main() {{
    const auto& resource=cs35l41Firmware[0];
    cirrus::devices::amplifiers::cs35l41::tuning::Parameters left, right;
    assert(cirrus::devices::amplifiers::cs35l41::tuning::parse(resource.tuningLeft,resource.tuningLeftSize,left));
    assert(cirrus::devices::amplifiers::cs35l41::tuning::parse(resource.tuningRight,resource.tuningRightSize,right));
    assert(left.pcmGain==14 && left.overridden);
    assert(right.pcmGain=={expected_right});
    assert(right.overridden=={str(expected_right != 17).lower()});
}}
''', encoding='utf-8')
        exe = tmp / 'profile.exe'
        subprocess.run(['g++','-std=c++17','-I',str(ROOT / 'CirrusAudioFixup'),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    compile_profile(16)
    run(["--bincfg-l", str(gain_left), "--firmware", str(header)], False)
    assert header.read_text(encoding='utf-8') == imported
    header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--bincfg-l", str(gain_left), "--firmware", str(header)], True)
    imported = header.read_text(encoding='utf-8')
    assert 'l0_bincfg), nullptr, 0,' in imported and 'r0_bincfg' not in imported
    compile_profile(17)

    # No guessed speaker ID; physical ID 0 can coexist with explicit generic
    # tuning and a shared byte-identical WMFW. Duplicates fail transactionally.
    command = command[:-2]
    header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--firmware", str(header)], False)
    run(["--generic-speaker", "--firmware", str(header)], True)
    run(["--spkid", "0", "--firmware", str(header)], True)
    stable = header.read_bytes()
    run(["--generic-speaker", "--firmware", str(header)], False)
    run(["--spkid", "0", "--firmware", str(header)], False)
    assert header.read_bytes() == stable
    # Omission of the right channel is not an implicit symmetric profile.
    original_command = command
    right_index = command.index('--bin-r')
    command = command[:right_index] + command[right_index+2:]
    symmetric = tmp/'Symmetric.hpp'
    symmetric.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n', encoding='utf-8')
    run(["--spkid", "1", "--firmware", str(symmetric)], False)
    run(["--spkid", "1", "--symmetric-tuning", "--firmware", str(symmetric)], True)
    command = original_command
    cpp = tmp / 'identity.cpp'
    cpp.write_text('#include <cassert>\n#include "Devices/Amplifiers/CS35L41/Resources/ResourceSchema.hpp"\n'
                   '#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"\n'
                   + header.read_text() + '''
int main(){
 using cirrus::devices::amplifiers::cs35l41::selectFirmwareResource;
 auto* zero=selectFirmwareResource(cs35l41Firmware,2,0x17aa,0x3847,0);
 auto* one=selectFirmwareResource(cs35l41Firmware,2,0x17aa,0x3847,1);
 auto* unknown=selectFirmwareResource(cs35l41Firmware,2,0x17aa,0x3847,UINT32_MAX);
 assert(zero&&!zero->genericSpeaker&&zero->spkid==0);
 assert(one&&one->genericSpeaker&&unknown==one&&zero!=one);
}
''', encoding='utf-8')
    exe = tmp / 'identity.exe'
    subprocess.run(['g++','-std=c++17','-I',str(ROOT/'CirrusAudioFixup'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

    # The default location must follow the production amplifier directory.
    # Exercise an isolated checkout so this test cannot change shipped assets.
    isolated = tmp/'checkout'
    isolated_tool = isolated/'Tools/import_firmware.py'
    isolated_tool.parent.mkdir(parents=True)
    shutil.copyfile(tool, isolated_tool)
    default_header = isolated/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/Resources/Firmware.hpp'
    default_header.parent.mkdir(parents=True)
    default_header.write_text('const FirmwareResource cs35l41Firmware[] = {\n};\n')
    unusual = tmp/'coeff,{source}.bin'
    unusual.write_bytes(left.read_bytes())
    isolated_command = [sys.executable,str(isolated_tool),*command[2:],
                        '--bin-l',str(unusual),'--generic-speaker']
    result = subprocess.run(isolated_command,capture_output=True,text=True)
    assert result.returncode == 0, result.stdout+result.stderr
    saved = default_header.read_bytes()
    result = subprocess.run(isolated_command,capture_output=True,text=True)
    assert result.returncode != 0 and 'already exists' in result.stderr
    assert default_header.read_bytes() == saved

collector = ROOT / "Tools/collect_macos.sh"
source = collector.read_text(encoding="utf-8")
assert "set -o pipefail" in source and "/bin/bash -o pipefail" in source
assert "header_size" in source and "empty output" in source
assert "-c AppleALC" not in source and "-c CS35L41" not in source
bash = shutil.which("bash")
if sys.platform == "win32":
    git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    bash = str(git_bash) if git_bash.exists() else None
assert bash, "Bash is required for collector syntax validation"
subprocess.run([bash, "-n", collector.as_posix()], check=True)
builder = ROOT / "Tools/build_macos.sh"
subprocess.run([bash, "-n", builder.as_posix()], check=True)
build_source = builder.read_text(encoding="utf-8")
assert 'lipo "$binary" -verify_arch x86_64' in build_source
assert '"$binary_uuid" == "$symbols_uuid"' in build_source
assert "kextload" not in build_source and "kmutil" not in build_source
if sys.platform != "darwin":
    denied = subprocess.run([bash, builder.as_posix()], capture_output=True, text=True)
    assert denied.returncode != 0 and "no bootable artifact produced" in denied.stderr
# Exercise the actual capture helpers in isolation. macOS system commands are
# deliberately not invoked by this host regression.
helpers = source[source.index("capture() {"):source.index("capture system ")]
with tempfile.TemporaryDirectory(prefix="cirrus-capture-") as directory:
    script = "output=.\ncapture_failed=0\n" + helpers + """
capture empty true
capture failed /bin/bash -c 'exit 7'
capture_shell failed-pipeline 'false | true'
true
"""
    subprocess.run([bash, "-c", script], cwd=directory, check=True)
    status = (Path(directory) / "status.txt").read_text(encoding="utf-8")
    assert "exit=7" in status and "exit=1" in status and "empty output" in status
assert "kernel-log-fallback" in source
print("PASS: firmware CLI regressions, collector syntax and capture failures (macOS execution not tested)")

# Execute the actual Desktop setup/EXIT trap without privileged captures. Stub
# account lookup only; mkdir/mktemp/chown/chmod operate on an isolated real tree.
if sys.platform != 'win32':
    import os
    setup = source[source.index('evidence_user='):source.index('capture() {')]
    setup = setup.replace('/usr/bin/stat', 'fixture_stat').replace('/usr/bin/id', 'fixture_id').replace('/usr/bin/dscl', 'fixture_dscl')
    setup = setup.replace('/usr/sbin/chown', shutil.which('chown')).replace('/usr/bin/mktemp', shutil.which('mktemp'))
    setup = setup.replace('/usr/bin/sudo', 'fixture_sudo')
    with tempfile.TemporaryDirectory(prefix='cirrus-desktop-') as directory:
        home = Path(directory) / 'user home'; home.mkdir()
        marker = Path(directory) / 'result-path'
        preamble = r'''
fixture_stat(){ printf '%s\n' tester; }
fixture_id(){ if [ "$1" = -u ]; then printf '%s\n' "$FIXTURE_UID"; else printf '%s\n' "$FIXTURE_GID"; fi; }
fixture_dscl(){ printf 'NFSHomeDirectory: %s\n' "$FIXTURE_HOME"; }
fixture_sudo(){
    shift 2
    case "$1" in
        /usr/bin/ditto)
            shift 4
            python3 -c 'import sys,zipfile,pathlib; p=pathlib.Path(sys.argv[1]); z=zipfile.ZipFile(sys.argv[2],"w"); [z.write(f,str(f.relative_to(p.parent))) for f in p.rglob("*") if f.is_file()]; z.close()' "$1" "$2" ;;
        /usr/bin/unzip)
            python3 -c 'import sys,zipfile; assert zipfile.ZipFile(sys.argv[1]).testzip() is None' "$3" ;;
        *) return 1 ;;
    esac
}
'''
        environment = dict(os.environ, FIXTURE_HOME=str(home), FIXTURE_UID=str(os.getuid()), FIXTURE_GID=str(os.getgid()), FIXTURE_MARKER=str(marker), SUDO_USER='tester')
        environment['HOME']='/var/root'
        for sudo_user, exit_code in (('tester',0),('root',7)):
            environment['SUDO_USER']=sudo_user
            script = preamble + setup + '\nprintf "%s" "$output" > "$FIXTURE_MARKER"\nprintf evidence > "$output/capture.txt"\nexit '+str(exit_code)+'\n'
            result = subprocess.run([bash,'-c',script],env=environment,capture_output=True,text=True)
            assert result.returncode == exit_code, result.stderr
            output = Path(marker.read_text())
            assert output.parent == home/'Desktop' and output.name.startswith('cirrus-evidence.')
            assert output.stat().st_uid == os.getuid() and output.stat().st_mode & 0o700 == 0o700
            capture = output/'capture.txt'
            assert capture.stat().st_uid == os.getuid() and capture.stat().st_mode & 0o600 == 0o600
            capture.write_text('user can edit')
            archive = Path(str(output)+'.zip')
            assert archive.is_file() and archive.stat().st_uid == os.getuid()
            assert archive.stat().st_mode & 0o777 == 0o600
            import zipfile
            with zipfile.ZipFile(archive) as bundle:
                assert bundle.read(output.name+'/capture.txt') == b'evidence'
print('PASS collector Desktop destination, sudo/console account resolution and user access after success/error exit')
