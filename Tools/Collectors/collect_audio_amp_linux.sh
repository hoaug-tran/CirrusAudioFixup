#!/usr/bin/env bash
# Collect Linux audio and amplifier data with bounded reads (Python 3, tar).
# Optional tracing configures a private ftrace instance.
set -Eeuo pipefail
if ! command -v python3 >/dev/null 2>&1; then echo 'python3 is required (no automatic installs)' >&2; exit 1; fi
exec python3 - "$@" <<'PY'
import argparse, datetime as dt, gzip, hashlib, json, os, pathlib, re, shutil, subprocess, sys, tarfile, tempfile
P = pathlib.Path
ap = argparse.ArgumentParser(description='Small, portable Linux audio / smart-AMP evidence bundle')
ap.add_argument('--output', default=None, help='Output directory (defaults to the invoking user home)')
ap.add_argument('--include-firmware', action='store_true', help='Copy matching firmware payloads, within size limits')
ap.add_argument('--no-acpi', action='store_true', help='Skip raw DSDT/SSDT tables')
ap.add_argument('--trace-seconds',type=int,default=0,help='Optional isolated, passive audio/regmap trace (0..15 seconds)')
ap.add_argument('--max-mb', type=int, default=24, help='Uncompressed collection limit in MiB (8..128)')
a = ap.parse_args()
if not 8 <= a.max_mb <= 128: ap.error('--max-mb must be 8..128')
if not 0 <= a.trace_seconds <= 15: ap.error('--trace-seconds must be 0..15')
os.umask(0o077)
uid = int(os.environ.get('SUDO_UID', os.getuid()))
try:
    import pwd
    user = pwd.getpwuid(uid); home = P(user.pw_dir); gid = user.pw_gid
except (ImportError, KeyError):
    uid = os.getuid(); gid = os.getgid(); home = P.home()
parent = P(a.output).expanduser() if a.output else home
parent = parent.resolve()
if not parent.is_dir(): ap.error('output directory does not exist')
stamp = dt.datetime.now().strftime('%Y%m%d-%H%M%S')
out = P(tempfile.mkdtemp(prefix='AUDIO_AMP_LINUX_'+stamp+'_', dir=str(parent)))
limit = a.max_mb * 1024 * 1024
used = 0
status = []
# Include driver/audio names and common ACPI/codec identity strings, not one OEM/AMP.
AMP = re.compile(r'cs35|clsa|csc35|cirrus|tas2[0-9a-z]+|tas5[0-9a-z]+|ti[-_ ]?smart|smart.?amp|amplifier|amp[-_ ]|max98[0-9a-z]+|max97[0-9a-z]+|rt13[0-9a-z]+|rt10[0-9a-z]+|aw8[0-9a-z]+|aw[0-9]{4,}|nau83[0-9a-z]+|es83[0-9a-z]+|snd[-_]soc|sof[-_]|soundwire|sdw[0-9]|speaker.?id|alc[0-9]{3,}', re.I)
AUDIO = re.compile(r'audio|sound|codec|hda|\bacp[0-9]\b|i2s|soundwire|sdw|amplifier|\bamp\b|intel.?sst|realtek|cirrus|maxim|nuvoton|ti[-_]|tas[0-9]', re.I)
DMI_KEYS = ('sys_vendor','product_name','product_version','board_vendor','board_name','bios_version')
SYS_KEYS = ('name','modalias','uevent','hid','uid','adr','status','path','vendor','device','subsystem_vendor','subsystem_device', 'driver_override','max_speed_hz','spi_max_frequency','bits_per_word','mode','irq')
def record(src, dest, state, message=''):
    status.append({'source':str(src), 'file':str(dest), 'state':state, 'note':str(message)})
def store(rel, data, src='', cap=262144):
    global used
    if isinstance(data, str): data = data.encode('utf-8', errors='replace')
    if cap is not None and len(data) > cap:
        data = data[:cap] + b'\n[TRUNCATED: per-file limit]\n'
        state = 'TRUNCATED'
    else: state = 'OK'
    if used + len(data) > limit:
        record(src, rel, 'SKIPPED', 'total size limit reached'); return False
    target = out / rel; target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data); used += len(data)
    record(src, rel, state, '%d bytes' % len(data)); return True

def read(src, rel, cap=262144):
    src = P(src)
    try:
        # Bounded reads; never recursively read arbitrary sysfs binary/control files.
        with src.open('rb') as f: data = f.read(cap + 1)
        return store(rel, data, str(src), cap)
    except (OSError, ValueError) as e:
        record(src, rel, 'UNAVAILABLE', str(e)); return False

def run(rel, command, timeout=10, cap=524288):
    if not shutil.which(command[0]):
        record(command[0],rel,'UNAVAILABLE','command not installed'); return
    try:
        p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, check=False)
        store(rel, p.stdout, ' '.join(command), cap)
        if p.returncode or p.stderr:
            store(rel+'.status.txt','returncode=%d\n%s' %(p.returncode,p.stderr[:4096].decode(errors='replace')), 'command-status', 8192)
    except (OSError, subprocess.TimeoutExpired) as e: record(' '.join(command), rel, 'UNAVAILABLE',str(e))

def safe_text(path, cap=4096):
    try:
        with P(path).open('rb') as f: return f.read(cap).decode(errors='replace').strip()
    except (OSError, UnicodeError): return ''

def node_identity(node):
    chunks=[node.name]
    for k in ('name','modalias','uevent','hid','status'):
        p=node/k
        if p.is_file(): chunks.append(safe_text(p,4096))
    try:
        drv=node/'driver'
        if drv.exists(): chunks.append(drv.resolve().name)
    except OSError: pass
    return ' '.join(chunks)

def node_info(node, bus, role):
    rel='devices/'+bus+'/'+re.sub(r'[^A-Za-z0-9_.-]','_',node.name)
    info={'bus':bus,'device':node.name,'role':role,'resolved_path':str(node.resolve()),'driver':None,'fields':{}}
    drv=node/'driver'
    try:
        if drv.exists(): info['driver']=drv.resolve().name
    except OSError: pass
    for key in SYS_KEYS:
        if (node/key).is_file(): info['fields'][key]=safe_text(node/key,16384)
    for key in ('runtime_status','control','runtime_active_time','runtime_suspended_time'):
        f=node/'power'/key
        if f.is_file(): info.setdefault('power',{})[key]=safe_text(f,256)
    store(rel+'.json',json.dumps(info,indent=2),str(node),32768)
    if shutil.which('udevadm'):
        run(rel+'.udev.txt',['udevadm','info','--query=property','--path='+str(node)],cap=16384)
    return info

try:
    store('system/identity.json',json.dumps({'utc':dt.datetime.now(dt.timezone.utc).isoformat(),
        'os':safe_text('/etc/os-release',4096),'kernel':os.uname().release,'arch':os.uname().machine,
        'dmi':{k:safe_text('/sys/class/dmi/id/'+k,512) for k in DMI_KEYS}},indent=2),'system',16384)
    # All ALSA cards/PCM/control names are useful for identifying devices even when the AMP is hidden.
    for name in ('cards','devices','modules','pcm'):
        f=P('/proc/asound')/name
        if f.is_file(): read(f,'alsa/'+name+'.txt',65536)
    for card in sorted(P('/proc/asound').glob('card[0-9]*')):
        for f in list(card.glob('codec#*')) + list(card.glob('eld#*')):
            if f.is_file(): read(f,'alsa/codec/'+card.name+'_'+f.name+'.txt',131072)
    run('alsa/aplay-list.txt',['aplay','-l'],cap=16384)
    run('alsa/arecord-list.txt',['arecord','-l'],cap=16384)
    run('alsa/mixer-controls.txt',['amixer','-c','0','contents'],cap=98304)
    # Cross-platform Linux sysfs buses. No kernel-specific bus is mandatory.
    nodes=[]; seen=set(); controllers=[]
    for bus in ('spi','i2c','sdw','soundwire','hdaudio','platform','acpi','pci'):
        base=P('/sys/bus')/bus/'devices'
        if not base.is_dir(): continue
        for node in sorted(base.iterdir()):
            identity=node_identity(node)
            score=bool(AMP.search(identity))
            if bus in ('spi','i2c','sdw','soundwire','hdaudio','acpi','platform') and AUDIO.search(identity): score=True
            # PCI audio controllers only. Never dump unrelated PCI devices.
            if bus=='pci': score=bool(re.search(r'0x040[13]|audio|snd[_-]|hda|sof[_-]',identity,re.I))
            if score:
                key=(bus,node.name)
                if key not in seen:
                    nodes.append((bus,node));seen.add(key)
                    if len(nodes)>=96: break
        if len(nodes)>=96: break
    inventory=[]
    for bus,node in nodes:
        try: inventory.append(node_info(node,bus,'AMP candidate' if AMP.search(node_identity(node)) else 'Audio/bus device'))
        except OSError as e: record(str(node),'devices','UNAVAILABLE',str(e))
    # Include physical bus controller path for discovered SPI/I2C/SDW devices, not every controller.
    for bus,node in nodes:
        if bus not in ('spi','i2c','sdw','soundwire'): continue
        p=node.resolve().parent
        for _ in range(4):
            if p==P('/sys/devices') or p==p.parent: break
            if (p/'uevent').is_file() or (p/'modalias').is_file():
                key=(bus,str(p))
                if key not in seen:
                    seen.add(key)
                    controllers.append({'child':node.name,'path':str(p),'name':safe_text(p/'name',256),
                        'modalias':safe_text(p/'modalias',512),'uevent':safe_text(p/'uevent',2048)})
                break
            p=p.parent
    store('devices/inventory.json',json.dumps({'matches':len(inventory),'devices':inventory,'bus_controllers':controllers},indent=2), 'inventory',524288)
    # ACPI AML is central to pin, GPIO, resource and _DSD routing. Capture DSDT/SSDT only.
    if not a.no_acpi:
        tablebase=P('/sys/firmware/acpi/tables')
        amount=0
        if tablebase.is_dir():
            for directory in (tablebase,tablebase/'dynamic'):
                if not directory.is_dir(): continue
                for f in sorted(directory.iterdir()):
                    if not re.fullmatch(r'DSDT|SSDT\d*',f.name,re.I) or not f.is_file(): continue
                    try: size=f.stat().st_size
                    except OSError: continue
                    if size>2*1024**2 or amount+size>6*1024**2:
                        record(f,'acpi','SKIPPED','ACPI table size budget'); continue
                    prefix='acpi/dynamic/' if directory!=tablebase else 'acpi/'
                    if read(f,prefix+f.name+'.aml',2*1024**2): amount+=size
        else: record(tablebase,'acpi','UNAVAILABLE','ACPI not exposed (e.g. ARM/VM/permissions)')
    # Read driver identifiers and loaded module metadata only; no full module/firmware tree.
    relevant=set()
    for item in inventory:
        if item.get('driver'): relevant.add(item['driver'])
    for m in sorted(relevant):
        run('drivers/'+re.sub(r'[^a-zA-Z0-9_.-]','_',m)+'.txt',['modinfo',m],cap=16384)
    # Regmap may be huge; only matched amplifier maps and a small bounded register snapshot.
    regbase=P('/sys/kernel/debug/regmap')
    count=0
    if regbase.is_dir():
        for entry in sorted(regbase.iterdir()):
            if not entry.is_dir() or not AMP.search(entry.name+' '+safe_text(entry/'name',512)): continue
            if count>=8: break
            label=re.sub(r'[^A-Za-z0-9_.-]','_',entry.name)
            for f in ('name','range','registers'):
                src=entry/f
                if src.is_file(): read(src,'regmap/'+label+'_'+f+'.txt',262144 if f=='registers' else 8192)
            count+=1
    else: record(regbase,'regmap','UNAVAILABLE','debugfs not mounted or restricted; no mount attempted')
    # Kernel ring buffer is collected only if it mentions these audio/AMP devices.
    if shutil.which('dmesg'):
        try:
            p=subprocess.run(['dmesg','--color=never'],capture_output=True,timeout=10)
            lines=p.stdout.decode(errors='replace').splitlines()
            pattern=re.compile(r'cs35|cirrus|tas[0-9]|max98|rt13|aw8[0-9]|nau8|speaker|amplifier|snd|sof|soundwire|audio|codec|hda|i2c|spi',re.I)
            filtered=[line for line in lines if pattern.search(line)]
            store('logs/kernel-audio.txt','\n'.join(filtered[-1200:])+'\n','dmesg',262144)
            if p.returncode: record('dmesg','logs/kernel-audio.txt','UNAVAILABLE',p.stderr.decode(errors='replace')[:200])
        except (OSError,subprocess.TimeoutExpired) as e: record('dmesg','logs/kernel-audio.txt','UNAVAILABLE',str(e))
    if a.trace_seconds:
        # Private ftrace instance. Only tracing controls are touched; no driver rebind or device writes.
        import time
        base=P('/sys/kernel/tracing')
        if not (base/'instances').is_dir(): base=P('/sys/kernel/debug/tracing')
        if not (base/'instances').is_dir():
            record('tracefs','trace','UNAVAILABLE','tracefs instance support not exposed')
        else:
            inst=base/'instances'/('audio_amp_'+str(os.getpid()))
            try:
                inst.mkdir()
                def control(name,value):
                    (inst/name).write_text(str(value)+'\n')
                try:
                    control('tracing_on','0')
                    control('buffer_size_kb','512')
                    enabled=0
                    for group in ('regmap','snd_soc','snd_soc_dapm','hda','soundwire','spi','i2c'):
                        f=inst/'events'/group/'enable'
                        if f.is_file():
                            try: f.write_text('1\n'); enabled+=1
                            except OSError: pass
                    if enabled:
                        control('tracing_on','1')
                        time.sleep(a.trace_seconds)
                        control('tracing_on','0')
                        raw=safe_text(inst/'trace',1024*1024)
                        store('trace/audio-events.txt',raw,'ftrace',524288)
                    else: record('tracefs','trace','UNAVAILABLE','no supported trace events')
                finally:
                    try: control('tracing_on','0')
                    except OSError: pass
                    try: control('events/enable','0')
                    except OSError: pass
                    try: inst.rmdir()
                    except OSError as e: record('tracefs',inst,'UNAVAILABLE','cleanup: '+str(e))
            except OSError as e: record('tracefs','trace','UNAVAILABLE',str(e))
    # List firmware names only by default: binary payloads are typically bulky and proprietary.
    fw=[]; count=0
    for root in ('/lib/firmware','/usr/lib/firmware'):
        if not P(root).is_dir(): continue
        for base,dirs,files in os.walk(root,followlinks=False):
            dirs[:]=[d for d in dirs if not (P(base)/d).is_symlink()]
            for name in files:
                path=P(base)/name; rel=str(path.relative_to(root))
                if not AMP.search(rel): continue
                try: size=path.stat().st_size
                except OSError: continue
                fw.append({'path':str(path),'bytes':size})
                if a.include_firmware and size<=2*1024**2 and count+size<=8*1024**2:
                    dest='firmware/'+re.sub(r'[^A-Za-z0-9_./-]','_',rel)
                    if read(path,dest,2*1024**2): count+=size
                if len(fw)>=256: break
            if len(fw)>=256: break
        if len(fw)>=256: break
    store('firmware/manifest.json',json.dumps(fw,indent=2),'firmware',65536)
except BaseException as e:
    record('collector','', 'ERROR',repr(e))
finally:
    store('README.txt','''Linux audio / AMP diagnostics. Auto-detects candidate devices; does not target HP/CS35L41.
Read-only: no installing dependencies, hardware writes, audio playback, rebind or probe.
Missing devices may be behind undocumented firmware; ACPI AML helps investigate those.
Output limits truncate oversized data; see status.json. No kernel tracing by default; --trace-seconds 5 enables isolated passive trace.
The archive may contain device identifiers and firmware; review before sharing.
Use sudo for restricted kernel/ACPI/regmap evidence. Optional --include-firmware.
''','README',4096)
    # Store the ledger before hashes; avoid hashing the checksum manifest itself.
    (out/'status.json').write_text(json.dumps(status,indent=2)+'\n')
    files=sorted(p for p in out.rglob('*') if p.is_file())
    sums=[]
    for file in files:
        digest=hashlib.sha256(file.read_bytes()).hexdigest()
        sums.append('%s  %s' %(digest,file.relative_to(out).as_posix()))
    (out/'SHA256SUMS').write_text('\n'.join(sums)+'\n')
    if os.geteuid()==0:
        for p in sorted(out.rglob('*'),key=lambda x:len(x.parts), reverse=True):
            try: os.chown(p,uid,gid,follow_symlinks=False)
            except OSError: pass
        try: os.chown(out,uid,gid)
        except OSError: pass
    archive=out.with_suffix('.tar.gz')
    with tarfile.open(archive,'w:gz',compresslevel=6) as tar:
        tar.add(out,arcname=out.name,recursive=True)
    if os.geteuid()==0:
        try: os.chown(archive,uid,gid)
        except OSError: pass
    print('Directory:',out)
    print('Archive:',archive)
    print('Collected bytes:',used,'Issues:',sum(x['state'] not in ('OK',) for x in status))
PY
